// The server's room on the room server: what SMS Launcher's Host Game does
// (sms-launcher src/rooms.js, HostRoom), so every launcher can find the
// server in its public list or by its room code and join it, with nothing
// new on the players' side.
//
// The room owns the UDP port players reach; the game listens on this
// computer only, on a port of its own, and the room passes packets between
// them: each player through a socket of its own (so the game sees each
// player at its own address), reached directly, by UDP hole punching the
// room server sets up, or through the room server's WebSocket (the relay)
// when neither works. Players who join with the address (no code) are
// passed on the same way.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace srv {

struct RoomOptions {
	uint16_t port = 24002;  // the UDP port players reach
	std::string name;       // the server's name in the list
	bool isPublic = true;   // in every launcher's list of public games (else joined by room code or address)
	std::string url;        // the room server
	std::function<void(const std::string&)> log;
};

class Room {
public:
	Room();
	~Room();
	// Opens `options.port` and chooses the game's own port; false with a sentence in `error`.
	bool start(const RoomOptions& options, std::string* error);
	uint16_t gamePort() const;
	void stop(); // closes the room (the room server ends it at once) and its sockets

	struct State;

private:
	std::unique_ptr<State> state_;
};

} // namespace srv
