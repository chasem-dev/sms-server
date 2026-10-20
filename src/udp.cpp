#include "udp.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace srv {

#ifdef _WIN32
using sock_t = SOCKET;
#else
using sock_t = int;
#endif

std::string ipText(uint32_t ip)
{
	char text[16];
	snprintf(text, sizeof text, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
	return text;
}

std::string addrText(const Addr& a) { return ipText(a.ip) + ":" + std::to_string(a.port); }

bool parseIp(const std::string& text, uint32_t* ip)
{
	unsigned a, b, c, d;
	char extra;
	if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return false;
	*ip = a << 24 | b << 16 | c << 8 | d;
	return true;
}

bool privateIp(uint32_t ip)
{
	return (ip >> 24) == 10 || (ip >> 16) == 0xC0A8 || (ip >> 20) == 0xAC1 || (ip >> 22) == (0x6440 >> 6);
}

void netInit()
{
#ifdef _WIN32
	static bool started = false;
	if (!started) {
		WSADATA data;
		started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}
#endif
}

static sockaddr_in socketAddress(uint32_t ip, uint16_t port)
{
	sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(ip);
	sa.sin_port = htons(port);
	return sa;
}

bool Udp::bind(uint32_t ip, uint16_t port, std::string* error)
{
	close();
	netInit();
	intptr_t fd = (intptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
	if (fd == (intptr_t)INVALID_SOCKET) {
#else
	if (fd < 0) {
#endif
		if (error)
			*error = "Couldn't make a UDP socket.";
		return false;
	}
#ifdef _WIN32
	// one program per port, as the launcher's exclusive bind
	BOOL exclusive = TRUE;
	setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof exclusive);
	// a player's game that closes must not stop this socket (WSAECONNRESET on the next read)
	BOOL off = FALSE;
	DWORD bytes = 0;
	WSAIoctl((SOCKET)fd, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
#endif
	sockaddr_in sa = socketAddress(ip, port);
	if (::bind((sock_t)fd, (sockaddr*)&sa, sizeof sa) != 0) {
		if (error)
			*error = port ? "UDP port " + std::to_string(port) + " is in use by another program. Close it, or choose another port."
			              : "Couldn't open a UDP port.";
#ifdef _WIN32
		closesocket((SOCKET)fd);
#else
		::close((int)fd);
#endif
		return false;
	}
#ifdef _WIN32
	u_long nonblocking = 1;
	ioctlsocket((SOCKET)fd, FIONBIO, &nonblocking);
#else
	fcntl((int)fd, F_SETFL, fcntl((int)fd, F_GETFL) | O_NONBLOCK);
#endif
	fd_ = fd;
	return true;
}

void Udp::close()
{
	if (fd_ == kNone)
		return;
#ifdef _WIN32
	closesocket((SOCKET)fd_);
#else
	::close((int)fd_);
#endif
	fd_ = kNone;
}

uint16_t Udp::port() const
{
	sockaddr_in sa;
	socklen_t size = sizeof sa;
	if (fd_ == kNone || getsockname((sock_t)fd_, (sockaddr*)&sa, &size) != 0)
		return 0;
	return ntohs(sa.sin_port);
}

void Udp::send(const void* data, size_t size, const Addr& to)
{
	if (fd_ == kNone)
		return;
	sockaddr_in sa = socketAddress(to.ip, to.port);
	sendto((sock_t)fd_, (const char*)data, (int)size, 0, (sockaddr*)&sa, sizeof sa);
}

int Udp::recv(void* data, size_t size, Addr* from)
{
	if (fd_ == kNone)
		return -1;
	for (;;) {
		sockaddr_in sa;
		socklen_t len = sizeof sa;
		int n = (int)recvfrom((sock_t)fd_, (char*)data, (int)size, 0, (sockaddr*)&sa, &len);
		if (n >= 0) {
			if (from) {
				from->ip = ntohl(sa.sin_addr.s_addr);
				from->port = ntohs(sa.sin_port);
			}
			return n;
		}
#ifdef _WIN32
		const int e = WSAGetLastError();
		if (e == WSAECONNRESET || e == WSAEMSGSIZE)
			continue; // an unreachable player, or a packet too big for us: the next one
#else
		if (errno == EINTR || errno == ECONNREFUSED)
			continue;
#endif
		return -1;
	}
}

bool resolve(const std::string& host, uint32_t* ip)
{
	if (parseIp(host, ip))
		return true;
	netInit();
	addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* found = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found)
		return false;
	*ip = ntohl(((sockaddr_in*)found->ai_addr)->sin_addr.s_addr);
	freeaddrinfo(found);
	return true;
}

// the launcher's lanAddresses: container, VM and virtual switch networks are left out
static bool virtualName(const char* name)
{
	static const char* const prefixes[] = {"docker", "br-", "veth", "virbr", "vboxnet", "vmnet", "lxc", "lxd",
	                                       "cni", "flannel", "podman", "vEthernet"};
	for (const char* p : prefixes)
#ifdef _WIN32
		if (_strnicmp(name, p, strlen(p)) == 0)
#else
		if (strncasecmp(name, p, strlen(p)) == 0)
#endif
			return true;
	return false;
}

static bool usableLan(uint32_t ip) { return (ip >> 24) != 127 && (ip >> 16) != 0xA9FE && ip != 0; }

std::vector<uint32_t> lanAddresses()
{
	std::vector<uint32_t> list;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buffer(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	if (GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size) == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(size);
		if (GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size) != NO_ERROR)
			return list;
	}
	for (auto* a = (IP_ADAPTER_ADDRESSES*)buffer.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		char name[256] = "";
		WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, nullptr, nullptr);
		if (virtualName(name))
			continue;
		for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
			const uint32_t ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
			if (usableLan(ip))
				list.push_back(ip);
		}
	}
#else
	ifaddrs* all = nullptr;
	if (getifaddrs(&all) != 0)
		return list;
	for (ifaddrs* a = all; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || virtualName(a->ifa_name))
			continue;
		const uint32_t ip = ntohl(((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr);
		if (usableLan(ip))
			list.push_back(ip);
	}
	freeifaddrs(all);
#endif
	return list;
}

bool waitReadable(const std::vector<intptr_t>& fds, std::vector<bool>* ready, int ms)
{
	ready->assign(fds.size(), false);
#ifdef _WIN32
	std::vector<WSAPOLLFD> list;
#else
	std::vector<pollfd> list;
#endif
	std::vector<size_t> which;
	for (size_t i = 0; i < fds.size(); i++)
		if (fds[i] >= 0) {
			list.push_back({});
			list.back().fd = (decltype(list.back().fd))fds[i];
			list.back().events = POLLIN;
			which.push_back(i);
		}
	if (list.empty()) {
#ifdef _WIN32
		Sleep(ms);
#else
		usleep(ms * 1000);
#endif
		return true;
	}
#ifdef _WIN32
	const int n = WSAPoll(list.data(), (ULONG)list.size(), ms);
#else
	const int n = poll(list.data(), list.size(), ms);
	if (n < 0 && errno == EINTR)
		return true;
#endif
	if (n < 0)
		return false;
	for (size_t i = 0; i < list.size(); i++)
		if (list[i].revents & (POLLIN | POLLERR | POLLHUP))
			(*ready)[which[i]] = true;
	return true;
}

} // namespace srv
