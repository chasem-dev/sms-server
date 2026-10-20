// UDP sockets, name lookup and this computer's addresses, the same on
// Windows (winsock) and everywhere else (BSD sockets). IPv4 only, as the
// game's multiplayer is.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace srv {

// An IPv4 address and port, both in host byte order.
struct Addr {
	uint32_t ip = 0;
	uint16_t port = 0;
	bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
	bool operator!=(const Addr& o) const { return !(*this == o); }
};

const uint32_t kLoopback = 0x7F000001; // 127.0.0.1

std::string ipText(uint32_t ip);
std::string addrText(const Addr& a);
bool parseIp(const std::string& text, uint32_t* ip);
// 10/8, 192.168/16, 172.16/12 and 100.64/10 (a VPN such as Tailscale), as the launcher's privateAddress
bool privateIp(uint32_t ip);

void netInit(); // winsock's start; harmless elsewhere

class Udp {
public:
	Udp() = default;
	~Udp() { close(); }
	Udp(const Udp&) = delete;
	Udp& operator=(const Udp&) = delete;

	// Binds to `ip`:`port` (0: any free port), non-blocking. False with a sentence in `error`.
	bool bind(uint32_t ip, uint16_t port, std::string* error = nullptr);
	void close();
	bool open() const { return fd_ != kNone; }
	uint16_t port() const; // the port it is bound to
	void send(const void* data, size_t size, const Addr& to);
	// One waiting packet: its size, or -1 when none is waiting.
	int recv(void* data, size_t size, Addr* from);
	intptr_t fd() const { return fd_; }

private:
	static const intptr_t kNone = -1;
	intptr_t fd_ = kNone;
};

// A host name's first IPv4 address (or an address as text). False when it has none.
bool resolve(const std::string& host, uint32_t* ip);

// This computer's addresses on its local networks (and VPNs), without loopback,
// link-local and the virtual networks of containers and virtual machines.
std::vector<uint32_t> lanAddresses();

// Waits up to `ms` for any of `fds` to have something to read (or, for a
// negative entry, ignores it); sets `ready[i]`. Returns false on an error.
bool waitReadable(const std::vector<intptr_t>& fds, std::vector<bool>* ready, int ms);

} // namespace srv
