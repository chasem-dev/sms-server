// Tests of what the server reads and writes besides the game: the room
// server's JSON, server.ini, STUN, the game's INFO answer and punching packets.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "json.h"
#include "packets.h"
#include "udp.h"

static int failures = 0;
#define CHECK(x)                                                        \
	do {                                                                \
		if (!(x)) {                                                     \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
			failures++;                                                 \
		}                                                               \
	} while (0)

using namespace srv;

static void jsonTests()
{
	Json v;
	CHECK(Json::parse(R"({"type":"join","id":7,"secret":"00ff","check":true,"candidates":[{"address":"1.2.3.4","port":5}],"name":"A\u00e9\"b"})", &v));
	CHECK(v["type"].text == "join");
	CHECK(v["id"].isInt() && v["id"].integer() == 7);
	CHECK(v["check"].boolean);
	CHECK(v["candidates"].items.size() == 1 && v["candidates"].items[0]["port"].integer() == 5);
	CHECK(v["name"].text == "A\xC3\xA9\"b");
	CHECK(v["missing"].type == Json::Null);
	CHECK(!Json::parse("{\"a\":1,}", &v));
	CHECK(!Json::parse("{\"a\":1} x", &v));
	CHECK(!Json::parse("pong", &v));
	CHECK(Json::parse("[1.5,-2,null,false]", &v) && v.items.size() == 4 && !v.items[0].isInt() && v.items[1].integer() == -2);
	CHECK(Json::parse("\"\\ud83c\\udf1e\"", &v) && v.text == "\xF0\x9F\x8C\x9E"); // a sun, from a surrogate pair
	CHECK(quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
	std::string deep(100, '[');
	CHECK(!Json::parse(deep, &v)); // too deep to be a room server's message
}

static void configTests()
{
	Config c;
	parseConfig("\xEF\xBB\xBF; comment\n# another\nName = \"Squid Race\"\nport=24010\npublic = no\nbogus = 1\nnot a setting\n", &c);
	CHECK(c.get("name") == "Squid Race");
	CHECK(c.number("port") == 24010);
	CHECK(!c.flag("public"));
	CHECK(c.flag("room_code")); // its default
	CHECK(c.get("versions") == "drift");
	CHECK(c.problems.size() == 2);
	c.dir = "/srv/sms";
	CHECK(c.pathOf("mods") == "/srv/sms/mods");
	c.values["disc"] = "/games/GMSE01.iso";
	CHECK(c.pathOf("disc") == "/games/GMSE01.iso");
	c.values["disc"] = "C:\\games\\GMSE01.iso";
	CHECK(c.pathOf("disc") == "C:\\games\\GMSE01.iso");
	CHECK(c.pathOf("stage").empty());

	// the file written the first time reads back as the defaults, with nothing wrong in it
	Config d;
	parseConfig(defaultConfigText(), &d);
	CHECK(d.problems.empty());
	for (const Setting& s : settings())
		CHECK(d.values.count(s.key) && d.values[s.key] == s.fallback);
}

static void stunTests()
{
	const uint8_t id[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	uint8_t request[20];
	stunRequest(id, request);
	CHECK(request[0] == 0 && request[1] == 1 && isStun(request, 20));
	// a binding response with XOR-MAPPED-ADDRESS 203.0.113.9:40000
	uint8_t r[32] = {0x01, 0x01, 0, 12, 0x21, 0x12, 0xA4, 0x42};
	memcpy(r + 8, id, 12);
	r[20] = 0x00, r[21] = 0x20, r[22] = 0, r[23] = 8;
	r[24] = 0, r[25] = 1;
	const uint16_t port = 40000 ^ 0x2112;
	r[26] = uint8_t(port >> 8), r[27] = uint8_t(port);
	const uint32_t ip = (203u << 24 | 0u << 16 | 113u << 8 | 9u) ^ kStunMagic;
	r[28] = uint8_t(ip >> 24), r[29] = uint8_t(ip >> 16), r[30] = uint8_t(ip >> 8), r[31] = uint8_t(ip);
	Addr mapped;
	CHECK(stunResponse(r, sizeof r, id, &mapped));
	CHECK(ipText(mapped.ip) == "203.0.113.9" && mapped.port == 40000);
	uint8_t other[12] = {};
	CHECK(!stunResponse(r, sizeof r, other, &mapped)); // another request's answer
	CHECK(!stunResponse(r, 24, id, &mapped));          // cut short
}

static void put32(std::vector<uint8_t>& p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p.push_back(uint8_t(v >> (8 * i)));
}
static void putStr(std::vector<uint8_t>& p, const std::string& s)
{
	p.push_back(uint8_t(s.size()));
	p.insert(p.end(), s.begin(), s.end());
}

static void infoTests()
{
	uint8_t probe[kProbeSize];
	probePacket(0x12345678, probe);
	CHECK(isGamePacket(probe, sizeof probe) && probe[6] == 1 && probe[7] == 0x78 && probe[10] == 0x12);

	// mp_net's INFO: header, players, most, open, build, name, status, u16 protocol, mode, server, version, ...
	std::vector<uint8_t> p;
	put32(p, kGameMagic);
	p.push_back(2), p.push_back(0), p.push_back(2);
	put32(p, 99);
	p.push_back(1), p.push_back(7), p.push_back(1);
	putStr(p, "8cedf488a093");
	putStr(p, "Squid Race");
	putStr(p, "Squid Race: waiting for players (1 of 2)");
	p.push_back(2), p.push_back(0);
	putStr(p, "squid-race");
	p.push_back(1);
	putStr(p, "2026.10.20-beta.8cedf48");
	p.push_back(1); // drift
	putStr(p, "2026.10.18");
	putStr(p, "0.1.63");
	p.push_back(0), p.push_back(0); // the least mod API, which the server doesn't read
	GameInfo info;
	CHECK(readInfo(p.data(), p.size(), 99, &info));
	CHECK(info.players == 1 && info.maxPlayers == 7 && info.open && info.build == "8cedf488a093");
	CHECK(info.status == "Squid Race: waiting for players (1 of 2)" && info.protocol == 2);
	CHECK(info.mode == "squid-race" && info.server && info.version == "2026.10.20-beta.8cedf48");
	CHECK(info.drift && info.minVersion == "2026.10.18" && info.minLauncher == "0.1.63");
	CHECK(!readInfo(p.data(), p.size(), 98, &info));  // another probe's answer
	CHECK(!readInfo(p.data(), 20, 99, &info));         // cut short
	// an older game's answer stops after the status
	std::vector<uint8_t> old(p.begin(), p.begin() + 11 + 3 + 1 + 12 + 1 + 10 + 1 + 40);
	CHECK(readInfo(old.data(), old.size(), 99, &info) && info.mode.empty() && info.version.empty() && !info.drift);
	// a game that says its version but not whom it takes
	std::vector<uint8_t> middle(p.begin(), p.begin() + 79 + 2 + 11 + 1 + 24);
	CHECK(readInfo(middle.data(), middle.size(), 99, &info) && info.version == "2026.10.20-beta.8cedf48" && !info.drift);
}

static void punchTests()
{
	uint8_t packet[kPunchSize];
	const std::string secret = "00112233445566778899aabbccddeeff";
	CHECK(punchPacket(kAck, secret, packet));
	int type = 0;
	std::string back;
	CHECK(readPunch(packet, sizeof packet, &type, &back) && type == kAck && back == secret);
	CHECK(!punchPacket(kHello, "00112233", packet));
	CHECK(!punchPacket(kHello, "zz112233445566778899aabbccddeeff", packet));
	CHECK(!readPunch(packet, 20, &type, &back));
}

static void addressTests()
{
	uint32_t ip;
	CHECK(parseIp("192.168.1.20", &ip) && privateIp(ip));
	CHECK(parseIp("100.100.1.2", &ip) && privateIp(ip)); // Tailscale
	CHECK(parseIp("172.20.0.1", &ip) && privateIp(ip));
	CHECK(parseIp("8.8.8.8", &ip) && !privateIp(ip));
	CHECK(!parseIp("1.2.3", &ip) && !parseIp("1.2.3.256", &ip) && !parseIp("1.2.3.4x", &ip));
}

int main()
{
	jsonTests();
	configTests();
	stunTests();
	infoTests();
	punchTests();
	addressTests();
	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	else
		printf("all checks passed\n");
	return failures ? 1 : 0;
}
