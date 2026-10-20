#include "packets.h"

#include <cstring>

namespace srv {

static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void stunRequest(const uint8_t id[12], uint8_t out[20])
{
	memset(out, 0, 20);
	out[1] = 0x01; // binding request, no attributes
	out[4] = 0x21;
	out[5] = 0x12;
	out[6] = 0xA4;
	out[7] = 0x42;
	memcpy(out + 8, id, 12);
}

bool isStun(const uint8_t* packet, size_t size) { return size >= 20 && be32(packet + 4) == kStunMagic; }

bool stunResponse(const uint8_t* packet, size_t size, const uint8_t id[12], Addr* mapped)
{
	if (size < 20 || be16(packet) != 0x0101 || be32(packet + 4) != kStunMagic || memcmp(packet + 8, id, 12) != 0)
		return false;
	const size_t end = size < 20u + be16(packet + 2) ? size : 20u + be16(packet + 2);
	bool found = false;
	for (size_t at = 20; at + 4 <= end;) {
		const uint16_t type = be16(packet + at), length = be16(packet + at + 2);
		const uint8_t* v = packet + at + 4;
		if (at + 4 + length > end)
			break;
		// family 1 (IPv4): 0, family, port, address
		if ((type == 0x0020 || (type == 0x0001 && !found)) && length >= 8 && v[1] == 1) {
			Addr a;
			a.port = be16(v + 2);
			a.ip = be32(v + 4);
			if (type == 0x0020) {
				a.port ^= uint16_t(kStunMagic >> 16);
				a.ip ^= kStunMagic;
			}
			*mapped = a;
			found = true;
			if (type == 0x0020)
				break;
		}
		at += 4 + length + ((4 - length % 4) % 4);
	}
	return found;
}

bool isGamePacket(const uint8_t* packet, size_t size) { return size >= 7 && le32(packet) == kGameMagic; }

void probePacket(uint32_t nonce, uint8_t out[kProbeSize])
{
	memset(out, 0, kProbeSize);
	const uint32_t magic = kGameMagic;
	for (int i = 0; i < 4; i++)
		out[i] = uint8_t(magic >> (8 * i));
	out[4] = 2; // the protocol the launcher's probe names; the game answers any
	out[6] = 1; // PROBE
	for (int i = 0; i < 4; i++)
		out[7 + i] = uint8_t(nonce >> (8 * i));
}

namespace {
struct Reader {
	const uint8_t* p;
	size_t size, at = 0;
	bool ok = true;
	uint8_t u8()
	{
		if (at + 1 > size) {
			ok = false;
			return 0;
		}
		return p[at++];
	}
	uint16_t u16()
	{
		if (at + 2 > size) {
			ok = false;
			return 0;
		}
		at += 2;
		return le16(p + at - 2);
	}
	uint32_t u32()
	{
		if (at + 4 > size) {
			ok = false;
			return 0;
		}
		at += 4;
		return le32(p + at - 4);
	}
	std::string str()
	{
		const size_t n = u8();
		if (!ok || at + n > size) {
			ok = false;
			return std::string();
		}
		at += n;
		return std::string((const char*)p + at - n, n);
	}
};
} // namespace

bool readInfo(const uint8_t* packet, size_t size, uint32_t nonce, GameInfo* info)
{
	Reader r{packet, size};
	if (r.u32() != kGameMagic)
		return false;
	const int protocol = r.u16();
	if (r.u8() != 2 || r.u32() != nonce || !r.ok) // INFO
		return false;
	GameInfo i;
	i.protocol = protocol;
	i.players = r.u8();
	i.maxPlayers = r.u8();
	i.open = r.u8() == 1;
	i.build = r.str();
	i.name = r.str();
	i.status = r.str();
	if (!r.ok)
		return false;
	if (r.at + 2 <= size)
		i.protocol = r.u16();
	if (r.at < size) {
		// a newer game's: its mode, whether it is a server, its version (and whom it takes, unread here)
		Reader rest = r;
		std::string mode = rest.str();
		const bool server = rest.u8() == 1;
		std::string version = rest.str();
		if (rest.ok) {
			i.mode = mode;
			i.server = server;
			i.version = version;
		}
	}
	if (i.maxPlayers < 1 || i.maxPlayers > 8 || i.players > i.maxPlayers)
		return false;
	*info = i;
	return true;
}

static int hexDigit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

bool punchPacket(PunchType type, const std::string& secretHex, uint8_t out[kPunchSize])
{
	if (secretHex.size() != 32)
		return false;
	memcpy(out, "SMLP", 4);
	out[4] = uint8_t(type);
	for (int i = 0; i < 16; i++) {
		const int hi = hexDigit(secretHex[2 * i]), lo = hexDigit(secretHex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out[5 + i] = uint8_t(hi << 4 | lo);
	}
	return true;
}

bool readPunch(const uint8_t* packet, size_t size, int* type, std::string* secretHex)
{
	if (size != kPunchSize || memcmp(packet, "SMLP", 4) != 0)
		return false;
	static const char digits[] = "0123456789abcdef";
	*type = packet[4];
	secretHex->clear();
	for (int i = 0; i < 16; i++) {
		*secretHex += digits[packet[5 + i] >> 4];
		*secretHex += digits[packet[5 + i] & 15];
	}
	return true;
}

} // namespace srv
