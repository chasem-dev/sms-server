// The packets the room client reads and writes besides the room server's
// messages: STUN binding requests (this port's public address), the game's
// probe and INFO answer (sms-pc-port ext/multiplayer/net/mp_net.h), and the
// launcher's hole-punching packets (sms-launcher src/rooms.js).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "udp.h"

namespace srv {

// --- STUN (RFC 5389): the address a STUN server saw a request come from
const uint32_t kStunMagic = 0x2112A442;
// A binding request with transaction id `id` (12 bytes); writes 20 bytes to `out`.
void stunRequest(const uint8_t id[12], uint8_t out[20]);
// A binding response to `id`: its XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS), when IPv4.
bool stunResponse(const uint8_t* packet, size_t size, const uint8_t id[12], Addr* mapped);
bool isStun(const uint8_t* packet, size_t size);

// --- The game's own packets: "SMMP", u16 protocol, u8 type
const uint32_t kGameMagic = 0x504D4D53; // "SMMP", little-endian
const int kProbeSize = 200;             // the game ignores a shorter probe
bool isGamePacket(const uint8_t* packet, size_t size);
void probePacket(uint32_t nonce, uint8_t out[kProbeSize]);

// The game's INFO answer to a probe.
struct GameInfo {
	int players = 0, maxPlayers = 0;
	bool open = false;
	std::string build, name, status;
	int protocol = 0;
	std::string mode;    // its game mode ("squid-race"), '' playing together
	bool server = false; // a dedicated server
	std::string version; // the game's version
	// whom it takes: other builds of its protocol (drift) or its own only, and the oldest versions
	bool drift = false;
	std::string minVersion, minLauncher;
};
bool readInfo(const uint8_t* packet, size_t size, uint32_t nonce, GameInfo* info);

// --- Hole punching: "SMLP", a type and the 16-byte secret of one join
enum PunchType { kHello = 1, kAck = 2, kKeep = 3 };
const size_t kPunchSize = 21;
// `secretHex` is the room server's 32 hex digits. False when it is not that.
bool punchPacket(PunchType type, const std::string& secretHex, uint8_t out[kPunchSize]);
// A punching packet's type and secret (as hex), or false.
bool readPunch(const uint8_t* packet, size_t size, int* type, std::string* secretHex);

} // namespace srv
