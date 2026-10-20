# How sms-server works

The server is the game: [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) built with this repository's sources added, running as a multiplayer host that nobody plays on.
Players' games talk to it exactly as they would to a friend hosting from SMS Launcher, which is why they need nothing new.

## The pieces

```
                     room server (sms-server-api)
                    ▲  wss: hello, status, joins, relay
                    │
 players ──UDP──▶ [ room ]──loopback──▶ [ the game: dedicated host + server mods ]
  (24002)          src/room.cpp            sms-pc-port, SMS_GX_BACKEND=none
                     ▲
                     │ server.ini → environment
               [ sms_server_init ]  src/server.cpp
```

- **`sms_server_init`** (`src/server.cpp`) is the first thing the game calls (`platform/port_runtime.cpp`, built in when the port is configured with `-DSMS_SERVER_DIR`).
  It reads `server.ini` (`src/config.cpp`) and sets the environment the game reads from then on: a headless dedicated host (`SMS_MULTIPLAYER=host`, `SMS_MULTIPLAYER_DEDICATED=1`), no renderer (`SMS_GX_BACKEND=none`), no sound, the mods folder, the name, the version policy and so on.
  Then it opens the room, and returns: the game starts as usual.
- **The game** boots, goes from the title to the first stage (where the mods take it, `stage`, or Delfino Plaza) and hosts there.
  Its dedicated-host behaviour is the port's own (ext/multiplayer): the first player to join is Mario, players arriving later join the stage in progress, and server mods (`side = server`) run here and direct everyone's game with server commands.
  With no renderer, nothing is drawn and the frame's read-backs see an empty frame; the game's logic runs as it does anywhere.
- **The room** (`src/room.cpp`) does what SMS Launcher's Host Game does ([`src/rooms.js`](https://github.com/chasem-dev/sms-launcher/blob/main/src/rooms.js)): the same messages, timings and fallbacks, so every launcher already knows how to find and join it.

## The room

The room owns the UDP port players reach (`port`); the game listens on a free port of its own on `127.0.0.1` only (`SMS_MULTIPLAYER_LOCAL=1`).
Each player gets a socket of its own on this computer, so the game sees each one at its own address, and the room passes packets between that socket and the player.

1. **Waiting for the game.** The room asks the game every second with the probe the launcher uses (`SMMP` PROBE), until the game answers INFO: it is hosting.
   INFO carries the game's build, version, protocol and mode, which the room tells the room server.
2. **The public address.** STUN binding requests (stunprotocol.org, Google, Cloudflare) from the players' port give its address as the internet sees it; with this computer's addresses on its local networks, those are the *candidates* players can try.
3. **The room server.** A WebSocket to `room_server/v1/rooms/new` (on a thread of its own, `src/ws.cpp`, over libcurl, so a slow room server never holds up the players' packets) sends `hello` (the candidates, name, build, version, protocol, whether it is public, the mode, and whom the game takes: `versions` drift or exact, `minVersion`, `minLauncher`, which launchers read from the list and the room code before joining) and gets a `room` back: the room code and a key that keeps the same code across reconnections.
4. **The list.** With `public = true`, every 20 seconds the room asks the game for INFO again and sends `status` (players, most players, the status line the mods set, whether it takes players): that is the server's line in every launcher's public list.
5. **Joins.** A player joining with the code makes the room server send `join` (the player's candidates and a secret for this join).
   Both sides then send `SMLP` HELLO packets carrying the secret to each other's candidates at once (UDP hole punching), so each router lets the other's packets in as replies.
   The first HELLO or ACK that arrives is the player's path; the room keeps it open (KEEP) until the player's game is on it.
   When punching fails, the room server says `relay`, and that player's packets go through the WebSocket instead (binary frames: the join's id, then the packet).
   A player joining with the address just sends game packets to the port, and gets a socket of its own the same way.
6. **Staying up.** A `ping` every 30 seconds keeps the WebSocket open; STUN is asked again every 25 seconds (a router can change the port's public address), and a changed address is sent in a new `hello`.
   A lost connection to the room server is tried again after 2, 5, 10, then every 30 seconds, with the same code where the room server still has it; players already playing are not affected.
7. **Stopping.** Ctrl+C (or SIGTERM) closes the WebSocket, and the room server ends the room at once, taking it out of the list.

## Files

| | |
| --- | --- |
| `server.cmake` | Included by the port's CMakeLists.txt: builds libcurl (HTTP and WebSockets only; Schannel on Windows, OpenSSL elsewhere) and `src/`, links them into the game, names it `sms-server` |
| `src/server.cpp` | `sms_server_init`: settings to environment, the room, Ctrl+C |
| `src/config.cpp` | `server.ini`: reading it, and writing it the first time |
| `src/room.cpp` | The room: the room server, STUN, punching, the relay, forwarding |
| `src/ws.cpp` | The room server's WebSocket, on its own thread |
| `src/udp.cpp` | UDP sockets, name lookup and local addresses on Windows and elsewhere |
| `src/packets.cpp` | STUN, the game's probe and INFO, punching packets |
| `src/json.cpp` | The room server's messages |
| `tests/` | Unit tests of the above that need no game |
| `tools/` | Packaging, and the start check CI runs |
