// The room server's WebSocket (wss://), on a thread of its own: connecting
// can take seconds when the room server is slow or away, and the room's
// packet forwarding must not wait for it. The room talks to it through
// queues; a wake-up packet to the room's loopback socket says there is news.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "udp.h"

namespace srv {

struct WsEvent {
	enum Kind { Open, Failed, Text, Binary, Closed } kind;
	std::string data; // Failed: why; Text, Binary: the message; Closed: the reason
	int code = 0;     // Closed: the close code (4404: the room ended)
};

class WebSocket {
public:
	// `wake` is told (an empty packet to 127.0.0.1:`wakePort`) whenever an event is waiting.
	explicit WebSocket(uint16_t wakePort);
	~WebSocket();

	void connect(const std::string& url); // replaces any connection
	void sendText(const std::string& text);
	void sendBinary(const void* data, size_t size);
	void close(int code = 1000); // and forget queued messages
	bool next(WsEvent* event);   // the next event, if any
	bool connected() const { return connected_; }

private:
	void run();
	void post(WsEvent event);

	struct Out {
		bool binary;
		std::string data;
	};
	void kick(); // wakes the thread: something to send
	uint16_t wakePort_;
	Udp wake_; // tells the room's socket
	Udp kick_; // the thread's own, to be told
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable changed_;
	std::deque<WsEvent> events_;
	std::deque<Out> out_;
	std::string url_;     // to connect to (generation_ says it is new)
	unsigned generation_ = 0;
	int closeCode_ = 0;   // to close with, when not 0
	bool quit_ = false;
	std::atomic<bool> connected_{false};
};

} // namespace srv
