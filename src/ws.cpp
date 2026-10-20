#include "ws.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#ifndef SMS_SERVER_VERSION
#define SMS_SERVER_VERSION "dev"
#endif

namespace srv {

static const long kConnectMs = 8000; // the launcher's REPLY_WAIT

// The CA certificates TLS checks the room server against. Windows (Schannel)
// uses the system's own; elsewhere the first bundle found where distributions
// keep it, or SSL_CERT_FILE.
static const char* caBundle()
{
#ifdef _WIN32
	return nullptr;
#else
	static const char* found = [] {
		if (const char* e = getenv("SSL_CERT_FILE"))
			return e;
		static const char* const paths[] = {
		    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Alpine
		    "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
		    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // RHEL
		    "/etc/ssl/ca-bundle.pem",                            // openSUSE
		    "/etc/ssl/cert.pem",                                 // macOS, Alpine
		};
		for (const char* p : paths)
			if (FILE* f = fopen(p, "r")) {
				fclose(f);
				return p;
			}
		return (const char*)nullptr;
	}();
	return found;
#endif
}

static bool waitSocket(curl_socket_t s, bool write, int ms)
{
#ifdef _WIN32
	WSAPOLLFD p = {};
	p.fd = s;
	p.events = write ? POLLOUT : POLLIN;
	return WSAPoll(&p, 1, ms) > 0;
#else
	pollfd p = {};
	p.fd = s;
	p.events = write ? POLLOUT : POLLIN;
	return poll(&p, 1, ms) > 0;
#endif
}

WebSocket::WebSocket(uint16_t wakePort) : wakePort_(wakePort)
{
	wake_.bind(kLoopback, 0);
	kick_.bind(kLoopback, 0);
	thread_ = std::thread([this] { run(); });
}

WebSocket::~WebSocket()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
		closeCode_ = 1000;
	}
	kick();
	if (thread_.joinable())
		thread_.join();
}

void WebSocket::connect(const std::string& url)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		url_ = url;
		generation_++;
		out_.clear();
		events_.clear(); // the last connection's news is old
		closeCode_ = 0;
	}
	kick();
}

void WebSocket::kick()
{
	changed_.notify_all();
	const char nothing = 0;
	wake_.send(&nothing, 0, {kLoopback, kick_.port()});
}

void WebSocket::sendText(const std::string& text)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		out_.push_back({false, text});
	}
	kick();
}

void WebSocket::sendBinary(const void* data, size_t size)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		out_.push_back({true, std::string((const char*)data, size)});
	}
	kick();
}

void WebSocket::close(int code)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		url_.clear();
		generation_++;
		out_.clear();
		closeCode_ = code;
	}
	kick();
}

bool WebSocket::next(WsEvent* event)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (events_.empty())
		return false;
	*event = std::move(events_.front());
	events_.pop_front();
	return true;
}

void WebSocket::post(WsEvent event)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		events_.push_back(std::move(event));
	}
	const char nothing = 0;
	wake_.send(&nothing, 0, {kLoopback, wakePort_});
}

void WebSocket::run()
{
	CURL* h = nullptr;
	curl_socket_t sock = CURL_SOCKET_BAD;
	unsigned generation = 0;
	std::string frame; // a message arriving in parts
	char buffer[16384];

	const auto drop = [&](int code) {
		if (!h)
			return;
		if (code) {
			unsigned char payload[2] = {(unsigned char)(code >> 8), (unsigned char)code};
			size_t sent = 0;
			curl_ws_send(h, payload, 2, &sent, 0, CURLWS_CLOSE);
		}
		curl_easy_cleanup(h);
		h = nullptr;
		sock = CURL_SOCKET_BAD;
		connected_ = false;
		frame.clear();
	};

	for (;;) {
		std::string url;
		bool reconnect = false;
		int closeCode = 0;
		std::deque<Out> out;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (!h && !quit_ && generation_ == generation)
				changed_.wait_for(lock, std::chrono::milliseconds(500));
			if (quit_) {
				closeCode = closeCode_ ? closeCode_ : 1000;
				lock.unlock();
				drop(closeCode);
				return;
			}
			if (generation_ != generation) {
				generation = generation_;
				url = url_;
				reconnect = true;
				closeCode = closeCode_;
			}
			out.swap(out_);
		}
		if (reconnect) {
			drop(closeCode ? closeCode : (h ? 1000 : 0));
			if (url.empty())
				continue;
			h = curl_easy_init();
			char error[CURL_ERROR_SIZE] = "";
			curl_easy_setopt(h, CURLOPT_URL, url.c_str());
			curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 2L); // the WebSocket upgrade, then ours
			curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, kConnectMs);
			curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, kConnectMs + 2000);
			curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(h, CURLOPT_USERAGENT, "sms-server/" SMS_SERVER_VERSION);
			curl_easy_setopt(h, CURLOPT_ERRORBUFFER, error);
			if (const char* ca = caBundle())
				curl_easy_setopt(h, CURLOPT_CAINFO, ca);
			const CURLcode r = curl_easy_perform(h);
			long status = 0;
			curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
			if (r != CURLE_OK || status != 101) {
				std::string why = r != CURLE_OK ? (error[0] ? error : curl_easy_strerror(r))
				                                : "the room server answered " + std::to_string(status);
				curl_easy_cleanup(h);
				h = nullptr;
				post({WsEvent::Failed, why});
				continue;
			}
			curl_easy_setopt(h, CURLOPT_ERRORBUFFER, nullptr);
			curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 0L);
			curl_easy_getinfo(h, CURLINFO_ACTIVESOCKET, &sock);
			connected_ = true;
			post({WsEvent::Open, std::string()});
			out.clear(); // anything queued before belonged to the last connection
		}
		if (!h)
			continue;

		bool lost = false;
		for (const Out& o : out) {
			size_t at = 0;
			int tries = 0;
			while (at < o.data.size() || (o.data.empty() && tries == 0)) {
				size_t sent = 0;
				const CURLcode r = curl_ws_send(h, o.data.data() + at, o.data.size() - at, &sent, 0,
				                                o.binary ? CURLWS_BINARY : CURLWS_TEXT);
				if (r == CURLE_AGAIN) {
					if (++tries > 50) {
						lost = true;
						break;
					}
					waitSocket(sock, true, 100);
					continue;
				}
				if (r != CURLE_OK) {
					lost = true;
					break;
				}
				at += sent;
				tries = 1;
			}
			if (lost)
				break;
		}

		// what arrived (or a kick: something to send): whole messages to the room
		std::vector<bool> ready;
		waitReadable({(intptr_t)sock, kick_.fd()}, &ready, 1000);
		char drain[16];
		while (kick_.recv(drain, sizeof drain, nullptr) >= 0) {
		}
		while (!lost) {
			size_t n = 0;
			const struct curl_ws_frame* meta = nullptr;
			const CURLcode r = curl_ws_recv(h, buffer, sizeof buffer, &n, &meta);
			if (r == CURLE_AGAIN)
				break;
			if (r != CURLE_OK || !meta) {
				lost = true;
				break;
			}
			if (meta->flags & CURLWS_CLOSE) {
				frame.append(buffer, n);
				if (meta->bytesleft)
					continue;
				const int code = frame.size() >= 2 ? ((unsigned char)frame[0] << 8 | (unsigned char)frame[1]) : 1005;
				const std::string reason = frame.size() > 2 ? frame.substr(2) : std::string();
				drop(0);
				post({WsEvent::Closed, reason, code});
				break;
			}
			if (meta->flags & (CURLWS_PING | CURLWS_PONG))
				continue; // curl answers pings itself
			frame.append(buffer, n);
			if (meta->bytesleft || (meta->flags & CURLWS_CONT))
				continue;
			post({(meta->flags & CURLWS_BINARY) ? WsEvent::Binary : WsEvent::Text, std::move(frame)});
			frame.clear();
		}
		if (lost && h) {
			drop(0);
			post({WsEvent::Closed, "the connection was lost", 1006});
		}
	}
}

} // namespace srv
