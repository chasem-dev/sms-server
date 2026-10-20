#include "room.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "json.h"
#include "packets.h"
#include "udp.h"
#include "ws.h"

namespace srv {

namespace {

// The launcher's timings (src/rooms.js), in ms
const int64_t PUNCH_EVERY = 100;   // between punches
const int64_t PUNCH_FOR = 5000;    // of punching before the joiner falls back to the relay
const int64_t KEEP_EVERY = 2000;   // a quiet path is kept open this often
const int64_t KEEP_FOR = 60000;    // after punching that a path is kept open with no game on it
const int64_t PEER_IDLE = 30000;   // before a silent player's forwarding is closed
const size_t MAX_PEERS = 12;       // players (and probes) forwarded at once
const int64_t PING_EVERY = 30000;  // between WebSocket keepalives
const int64_t RESTUN_EVERY = 25000; // between checks of the public address
const int64_t STATUS_EVERY = 20000; // between a public room's reports of its game for the list
const int64_t STATUS_RETRY = 5000;  // between tries while the game isn't answering
const int64_t REPLY_WAIT = 8000;   // for the room server
const int64_t RECONNECT[] = {2000, 5000, 10000, 30000};
const int64_t TICK = 250;
const int64_t STUN_FOR = 2000;     // waiting for a STUN server's answer
const int64_t PROBE_EVERY = 1000;  // asking the game until it is hosting

struct StunServer {
	const char* host;
	uint16_t port;
};
const StunServer STUN_SERVERS[] = {{"stun.stunprotocol.org", 3478}, {"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}};

int64_t now()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t key(const Addr& a) { return uint64_t(a.ip) << 16 | a.port; }

std::string pathName(uint32_t ip) { return privateIp(ip) ? "over the local network" : "directly over the internet"; }

// the launcher's playerName: printable, at most 23 characters, else `fallback`
std::string playerName(const std::string& name, const char* fallback)
{
	std::string out;
	for (unsigned char c : name)
		if (c >= 0x20 && c != 0x7F)
			out += char(c);
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	size_t start = out.find_first_not_of(' ');
	out = start == std::string::npos ? std::string() : out.substr(start);
	if (out.size() > 23)
		out.resize(23);
	return out.empty() ? fallback : out;
}

std::string formatCode(const std::string& code) { return code.size() == 6 ? code.substr(0, 3) + "-" + code.substr(3) : code; }

} // namespace

struct Peer;

struct Join {
	uint32_t id = 0;
	std::string secret, name;
	std::vector<Addr> candidates;
	bool check = false;
	int64_t started = 0, punchedAt = 0;
	bool reached = false;
	Addr reachedAt;
	Peer* peer = nullptr;
};

// A player the room passes packets for: its own socket on this computer.
struct Peer {
	Udp local;
	bool relay = false;
	uint32_t relayId = 0;
	Addr route;
	int64_t heard = 0, sent = 0;
};

struct Room::State {
	RoomOptions o;
	std::atomic<bool> quit{false};
	std::thread thread;

	Udp socket;  // the port players reach
	Udp wake;    // the WebSocket thread's news
	Udp probe;   // asks the game how it is
	uint16_t gamePort = 0;
	std::unique_ptr<WebSocket> ws;

	// the game, as its probe answers
	GameInfo info;
	bool hosting = false;
	uint32_t probeNonce = 0;
	int64_t probeSentAt = 0, probeDeadline = 0;
	bool probing = false;
	bool probeForStatus = false;

	// where players can reach the port
	std::vector<Addr> candidates;
	std::vector<Addr> stunTargets;
	std::vector<std::array<uint8_t, 12>> stunIds;
	int64_t stunUntil = 0, stunSentAt = 0, stunAt = 0;
	bool stunning = false, firstStun = true;

	// the room
	enum { Waiting, Connecting, AwaitingRoom, Open, Backoff } phase = Waiting;
	std::string code, roomKey;
	int attempt = 0;
	int64_t phaseUntil = 0, pingAt = 0, statusAt = 0;
	bool listed = false;

	std::map<uint32_t, Join> joins;
	std::vector<std::unique_ptr<Peer>> peers;
	std::map<uint64_t, Peer*> byAddress;
	std::mt19937 random{std::random_device{}()};

	void log(const std::string& text)
	{
		if (o.log)
			o.log(text);
	}

	void run();
	void tick(int64_t t);
	void startStun(int64_t t);
	void fromStun(const uint8_t* p, int n);
	void stunDone();
	void askGame(int64_t t, bool forStatus);
	void fromGameProbe(const uint8_t* p, int n);
	void connect(int64_t t);
	void reconnectLater(int64_t t);
	void sendHello();
	void fromServer(const WsEvent& e, int64_t t);
	void fromServerMessage(const Json& msg, int64_t t);
	void fromInternet(const uint8_t* p, int n, const Addr& from, int64_t t);
	Peer* peerFor(Join& join, const Addr* route);
	Peer* addPeer(const Addr* route, uint32_t relayId);
	void dropPeer(Peer* peer);
	void toGame(Peer* peer, const uint8_t* p, int n, int64_t t);
	void toInternet(Peer* peer, const uint8_t* p, int n, int64_t t);
};

// ---------------------------------------------------------------- the game

void Room::State::askGame(int64_t t, bool forStatus)
{
	probeNonce = random() | 1;
	uint8_t packet[kProbeSize];
	probePacket(probeNonce, packet);
	probe.send(packet, sizeof packet, {kLoopback, gamePort});
	probing = true;
	probeForStatus = forStatus;
	probeSentAt = t;
	probeDeadline = t + 1500;
}

void Room::State::fromGameProbe(const uint8_t* p, int n)
{
	GameInfo answer;
	if (!probing || !readInfo(p, (size_t)n, probeNonce, &answer))
		return;
	probing = false;
	info = answer;
	if (!hosting) {
		hosting = true;
		log("The game is hosting: opening the room.");
		startStun(now());
		return;
	}
	if (probeForStatus && phase == Open && o.isPublic) {
		statusAt = now() + STATUS_EVERY;
		ws->sendText(std::string("{\"type\":\"status\",\"players\":") + std::to_string(info.players) +
		             ",\"maxPlayers\":" + std::to_string(info.maxPlayers) + ",\"status\":" + quote(info.status) +
		             ",\"open\":" + (info.open ? "true" : "false") + "}");
		if (!listed) {
			listed = true;
			log("The server is in the public list: anyone can join it from their Multiplayer window.");
		}
	}
}

// ---------------------------------------------------------------- STUN: the port's public address

void Room::State::startStun(int64_t t)
{
	if (stunTargets.empty())
		for (const StunServer& s : STUN_SERVERS) {
			uint32_t ip;
			if (resolve(s.host, &ip))
				stunTargets.push_back({ip, s.port});
		}
	stunIds.assign(stunTargets.size(), {});
	for (auto& id : stunIds)
		for (auto& b : id)
			b = uint8_t(random());
	stunning = true;
	stunAt = t;
	stunUntil = t + STUN_FOR;
	stunSentAt = 0;
	if (stunTargets.empty())
		stunDone();
}

void Room::State::fromStun(const uint8_t* p, int n)
{
	if (!stunning)
		return;
	for (size_t i = 0; i < stunIds.size(); i++) {
		Addr mapped;
		if (stunResponse(p, (size_t)n, stunIds[i].data(), &mapped)) {
			std::vector<Addr> list{mapped};
			const uint16_t port = socket.port();
			for (uint32_t ip : lanAddresses())
				list.push_back({ip, port});
			std::vector<Addr> unique;
			for (const Addr& a : list) {
				bool seen = false;
				for (const Addr& u : unique)
					seen = seen || u == a;
				if (!seen && unique.size() < 6)
					unique.push_back(a);
			}
			stunning = false;
			const bool changed = unique.size() != candidates.size() || !std::equal(unique.begin(), unique.end(), candidates.begin());
			candidates = unique;
			if (firstStun)
				stunDone();
			else if (changed && phase == Open)
				sendHello();
			return;
		}
	}
}

// the first look at the public address is done (or gave up): to the room server
void Room::State::stunDone()
{
	stunning = false;
	if (candidates.empty()) {
		const uint16_t port = socket.port();
		for (uint32_t ip : lanAddresses())
			if (candidates.size() < 6)
				candidates.push_back({ip, port});
		log("No STUN server answered: players outside this network may only reach the server through the relay.");
	}
	if (firstStun) {
		firstStun = false;
		connect(now());
	}
}

// ---------------------------------------------------------------- the room server

void Room::State::connect(int64_t t)
{
	std::string url = o.url;
	if (url.compare(0, 5, "https") == 0)
		url = "wss" + url.substr(5);
	else if (url.compare(0, 4, "http") == 0)
		url = "ws" + url.substr(4);
	url += "/v1/rooms/new";
	if (!code.empty() && !roomKey.empty())
		url += "?code=" + code + "&key=" + roomKey;
	phase = Connecting;
	phaseUntil = t + REPLY_WAIT + 4000;
	ws->connect(url);
}

void Room::State::reconnectLater(int64_t t)
{
	const int64_t wait = RECONNECT[attempt < 3 ? attempt : 3];
	attempt++;
	phase = Backoff;
	phaseUntil = t + wait;
	listed = false;
}

void Room::State::sendHello()
{
	std::string list = "[";
	for (size_t i = 0; i < candidates.size(); i++)
		list += std::string(i ? "," : "") + "{\"address\":" + quote(ipText(candidates[i].ip)) +
		        ",\"port\":" + std::to_string(candidates[i].port) + "}";
	list += "]";
	ws->sendText("{\"type\":\"hello\",\"candidates\":" + list + ",\"name\":" + quote(o.name) +
	             ",\"build\":" + quote(info.build) + ",\"version\":" + quote(info.version) +
	             ",\"launcher\":\"\",\"protocol\":" + std::to_string(info.protocol) +
	             ",\"public\":" + (o.isPublic ? "true" : "false") + ",\"mode\":" + quote(info.mode) + "}");
}

void Room::State::fromServer(const WsEvent& e, int64_t t)
{
	switch (e.kind) {
	case WsEvent::Open:
		phase = AwaitingRoom;
		phaseUntil = t + REPLY_WAIT;
		sendHello();
		break;
	case WsEvent::Failed:
		if (code.empty() && attempt == 0)
			log("Room codes aren't available right now (" + e.data + "). Players can still join with the address.");
		reconnectLater(t);
		break;
	case WsEvent::Closed:
		if (phase == AwaitingRoom && e.code == 4400)
			log("The room server didn't take the server's hello (" + e.data + ").");
		reconnectLater(t);
		break;
	case WsEvent::Binary: {
		if (e.data.size() <= 4)
			break;
		const uint8_t* d = (const uint8_t*)e.data.data();
		const uint32_t id = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24;
		auto it = joins.find(id);
		if (it != joins.end() && it->second.peer && it->second.peer->relay)
			toGame(it->second.peer, d + 4, (int)e.data.size() - 4, t);
		break;
	}
	case WsEvent::Text: {
		if (e.data == "pong")
			break;
		Json msg;
		if (Json::parse(e.data, &msg) && msg.type == Json::Object)
			fromServerMessage(msg, t);
		break;
	}
	}
}

static bool hexSecret(const std::string& s)
{
	if (s.size() != 32)
		return false;
	for (char c : s)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	return true;
}

void Room::State::fromServerMessage(const Json& msg, int64_t t)
{
	const std::string type = msg["type"].text;
	if (type == "room" && msg["code"].type == Json::String) {
		const std::string newCode = msg["code"].text;
		const bool changed = !code.empty() && code != newCode;
		code = newCode;
		roomKey = msg["key"].text;
		attempt = 0;
		phase = Open;
		pingAt = t;
		statusAt = 0; // a new connection's room needs its line in the list again
		log(changed ? "The room code changed to " + formatCode(code) + "."
		            : "Room code " + formatCode(code) + ": players join with it from their Multiplayer window" +
		                  (o.isPublic ? ", or from the public list." : "."));
	} else if (type == "join" && msg["id"].isInt() && msg["secret"].type == Json::String && hexSecret(msg["secret"].text) &&
	           msg["candidates"].type == Json::Array) {
		Join j;
		j.id = (uint32_t)msg["id"].integer();
		j.secret = msg["secret"].text;
		j.check = msg["check"].type == Json::Bool && msg["check"].boolean;
		j.name = playerName(msg["name"].text, "A player");
		j.started = t;
		for (const Json& c : msg["candidates"].items) {
			uint32_t ip;
			if (parseIp(c["address"].text, &ip) && c["port"].isInt() && c["port"].integer() > 0 && c["port"].integer() < 65536)
				j.candidates.push_back({ip, (uint16_t)c["port"].integer()});
		}
		if (!j.check)
			log(j.name + " is joining with the room code...");
		joins[j.id] = j;
	} else if (type == "relay" && msg["id"].isInt()) {
		auto it = joins.find((uint32_t)msg["id"].integer());
		if (it == joins.end())
			return;
		Join& j = it->second;
		Peer* peer = peerFor(j, nullptr);
		if (!peer)
			return;
		peer->relay = true;
		peer->relayId = j.id;
		if (!j.check)
			log(msg["reason"].text == "lost"
			        ? j.name + "'s direct connection stopped working: now through the relay, which adds a little delay."
			        : j.name + " is connected through the relay, which adds a little delay: the networks couldn't connect "
			                   "directly. Forwarding UDP port " + std::to_string(o.port) + " to this computer lets players connect directly.");
	} else if (type == "leave" && msg["id"].isInt()) {
		auto it = joins.find((uint32_t)msg["id"].integer());
		if (it == joins.end())
			return;
		// a check's forwarding goes with it, so checks never take a joining player's place
		if (it->second.peer && (it->second.peer->relay || it->second.check))
			dropPeer(it->second.peer);
		joins.erase(it);
	}
}

// ---------------------------------------------------------------- forwarding

Peer* Room::State::addPeer(const Addr* route, uint32_t relayId)
{
	if (peers.size() >= MAX_PEERS)
		return nullptr;
	std::unique_ptr<Peer> peer(new Peer);
	if (!peer->local.bind(kLoopback, 0))
		return nullptr;
	if (route)
		peer->route = *route;
	else {
		peer->relay = true;
		peer->relayId = relayId;
	}
	peer->heard = peer->sent = now();
	peers.push_back(std::move(peer));
	return peers.back().get();
}

Peer* Room::State::peerFor(Join& join, const Addr* route)
{
	bool alive = false;
	for (auto& p : peers)
		alive = alive || p.get() == join.peer;
	if (!join.peer || !alive)
		join.peer = addPeer(route, join.id);
	return join.peer;
}

void Room::State::dropPeer(Peer* peer)
{
	for (auto it = byAddress.begin(); it != byAddress.end();)
		it = it->second == peer ? byAddress.erase(it) : std::next(it);
	for (auto& j : joins)
		if (j.second.peer == peer)
			j.second.peer = nullptr;
	for (auto it = peers.begin(); it != peers.end(); ++it)
		if (it->get() == peer) {
			peers.erase(it);
			break;
		}
}

void Room::State::toGame(Peer* peer, const uint8_t* p, int n, int64_t t)
{
	peer->heard = t;
	peer->local.send(p, (size_t)n, {kLoopback, gamePort});
}

void Room::State::toInternet(Peer* peer, const uint8_t* p, int n, int64_t t)
{
	peer->sent = t;
	if (peer->relay) {
		std::string frame(4 + (size_t)n, '\0');
		for (int i = 0; i < 4; i++)
			frame[i] = char(peer->relayId >> (8 * i));
		memcpy(&frame[4], p, (size_t)n);
		ws->sendBinary(frame.data(), frame.size());
	} else
		socket.send(p, (size_t)n, peer->route);
}

void Room::State::fromInternet(const uint8_t* p, int n, const Addr& from, int64_t t)
{
	if (isStun(p, (size_t)n)) {
		fromStun(p, n);
		return;
	}
	int punchType;
	std::string secret;
	if (readPunch(p, (size_t)n, &punchType, &secret)) {
		Join* join = nullptr;
		for (auto& j : joins)
			if (j.second.secret == secret)
				join = &j.second;
		if (!join)
			return;
		if (punchType == kHello) {
			uint8_t ack[kPunchSize];
			if (punchPacket(kAck, join->secret, ack))
				socket.send(ack, sizeof ack, from);
		}
		if (punchType != kHello && punchType != kAck)
			return;
		Peer* peer = peerFor(*join, &from);
		if (!peer)
			return;
		byAddress[key(from)] = peer;
		if (!join->reached) {
			join->reached = true;
			join->reachedAt = from;
			if (!join->check)
				log(join->name + " is connected " + pathName(from.ip) + ".");
		}
		return;
	}
	if (!isGamePacket(p, (size_t)n))
		return;
	Peer* peer = nullptr;
	auto it = byAddress.find(key(from));
	if (it != byAddress.end())
		peer = it->second;
	else {
		// someone joining with the address rather than the code
		peer = addPeer(&from, 0);
		if (!peer)
			return;
		byAddress[key(from)] = peer;
	}
	// answer from wherever the player sends from now
	if (!peer->relay)
		peer->route = from;
	toGame(peer, p, n, t);
}

// ---------------------------------------------------------------- the loop

void Room::State::tick(int64_t t)
{
	// until the game hosts, ask it every second
	if (!hosting) {
		if (!probing || t >= probeDeadline) {
			if (t - probeSentAt >= PROBE_EVERY)
				askGame(t, false);
		}
		return;
	}
	if (probing && t >= probeDeadline) {
		probing = false;
		if (probeForStatus)
			statusAt = t + STATUS_RETRY;
	}

	if (stunning) {
		if (t >= stunUntil) {
			stunning = false;
			if (firstStun)
				stunDone();
		} else if (t - stunSentAt >= 500) {
			stunSentAt = t;
			for (size_t i = 0; i < stunTargets.size(); i++) {
				uint8_t request[20];
				stunRequest(stunIds[i].data(), request);
				socket.send(request, sizeof request, stunTargets[i]);
			}
		}
	}

	if ((phase == Connecting || phase == AwaitingRoom) && t >= phaseUntil) {
		ws->close(1000);
		if (code.empty() && attempt == 0)
			log("Room codes aren't available right now (the room server didn't answer). Players can still join with the address.");
		reconnectLater(t);
	}
	if (phase == Backoff && t >= phaseUntil)
		connect(t);

	for (auto it = joins.begin(); it != joins.end();) {
		Join& j = it->second;
		const int64_t age = t - j.started;
		// punch while the joiner does; then keep the path open until their game is on it
		if (!j.reached && age < PUNCH_FOR + 1000 && t - j.punchedAt >= PUNCH_EVERY) {
			j.punchedAt = t;
			uint8_t hello[kPunchSize];
			if (punchPacket(kHello, j.secret, hello))
				for (const Addr& target : j.candidates)
					socket.send(hello, sizeof hello, target);
		} else if (j.reached && age < KEEP_FOR && t - (j.peer ? j.peer->sent : 0) >= KEEP_EVERY && t - j.punchedAt >= KEEP_EVERY) {
			j.punchedAt = t;
			uint8_t keep[kPunchSize];
			if (punchPacket(kKeep, j.secret, keep))
				socket.send(keep, sizeof keep, j.reachedAt);
		}
		if (!j.peer && age > 10 * 60 * 1000)
			it = joins.erase(it);
		else
			++it;
	}
	for (size_t i = 0; i < peers.size();) {
		Peer* p = peers[i].get();
		if (t - std::max(p->heard, p->sent) > PEER_IDLE)
			dropPeer(p);
		else
			i++;
	}

	if (phase == Open) {
		// the public list's line: the game's own answer to a probe
		if (o.isPublic && !probing && t >= statusAt)
			askGame(t, true);
		if (t - pingAt >= PING_EVERY) {
			pingAt = t;
			ws->sendText("ping");
		}
		// the router may give the port another public address after a quiet while
		if (!stunning && t - stunAt >= RESTUN_EVERY)
			startStun(t);
	}
}

void Room::State::run()
{
	std::vector<uint8_t> buffer(65536);
	int64_t nextTick = 0;
	while (!quit) {
		std::vector<intptr_t> fds{socket.fd(), wake.fd(), probe.fd()};
		for (auto& p : peers)
			fds.push_back(p->local.fd());
		const int64_t t0 = now();
		std::vector<bool> ready;
		waitReadable(fds, &ready, int(std::max<int64_t>(1, std::min<int64_t>(TICK, nextTick - t0))));
		const int64_t t = now();
		Addr from;
		int n;
		while ((n = socket.recv(buffer.data(), buffer.size(), &from)) >= 0)
			fromInternet(buffer.data(), n, from, t);
		while ((n = probe.recv(buffer.data(), buffer.size(), &from)) >= 0)
			if (from.ip == kLoopback && from.port == gamePort)
				fromGameProbe(buffer.data(), n);
		for (size_t i = 0; i < peers.size(); i++) {
			Peer* p = peers[i].get();
			while ((n = p->local.recv(buffer.data(), buffer.size(), &from)) >= 0)
				if (from.ip == kLoopback && from.port == gamePort)
					toInternet(p, buffer.data(), n, t);
		}
		while (wake.recv(buffer.data(), buffer.size(), nullptr) >= 0) {
		}
		WsEvent e;
		while (ws->next(&e))
			fromServer(e, t);
		if (t >= nextTick) {
			nextTick = t + TICK;
			tick(t);
		}
	}
	ws->close(1000);
	ws.reset(); // sends the close, so the room server ends the room at once
	peers.clear();
	socket.close();
}

// ---------------------------------------------------------------- Room

Room::Room() {}
Room::~Room() { stop(); }

uint16_t Room::gamePort() const { return state_ ? state_->gamePort : 0; }

bool Room::start(const RoomOptions& options, std::string* error)
{
	stop();
	netInit();
	std::unique_ptr<State> s(new State);
	s->o = options;
	if (!s->socket.bind(0, options.port, error))
		return false;
	// a port on this computer that nothing is using, for the game
	Udp free;
	if (!free.bind(kLoopback, 0, error))
		return false;
	s->gamePort = free.port();
	free.close();
	if (!s->wake.bind(kLoopback, 0, error) || !s->probe.bind(kLoopback, 0, error))
		return false;
	s->ws.reset(new WebSocket(s->wake.port()));
	State* raw = s.get();
	s->thread = std::thread([raw] { raw->run(); });
	state_ = std::move(s);
	return true;
}

void Room::stop()
{
	if (!state_)
		return;
	state_->quit = true;
	if (state_->thread.joinable())
		state_->thread.join();
	state_.reset();
}

} // namespace srv
