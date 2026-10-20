# sms-server

A dedicated server for [Super Mario Sunshine on PC](https://github.com/chasem-dev/sms-pc-port)'s multiplayer: run it on a spare computer or a VPS and players join it from [SMS Launcher](https://github.com/chasem-dev/sms-launcher) like any game, from the public list or with its room code.
Game modes are mods that run on the server only, such as the [Squid Race](https://github.com/chasem-dev/sms-squid-race-server), so players need nothing but the game.

It is the game itself, built without a window: it draws nothing, plays no sound, and uses about 70 MB of memory and a fraction of one CPU core.

## Running a server

1. Download the server for your system from [Releases](../../releases):
   - **beta** matches the game in SMS Launcher's beta, and is rebuilt when that changes;
   - **v&lt;version&gt;** (the latest release) matches the launcher's regular game.

   Players need the same game as the server (see [Versions](#versions)), so pick the one your players run.
2. Unzip it, and put your Super Mario Sunshine (USA, GMSE01) disc image next to `sms-server`: `.iso`, `.gcm` or Dolphin `.ciso`.
3. Start it: `./sms-server` on Linux, `sms-server.exe` on Windows.
   The first start writes [`server.ini`](#serverini) with every setting explained; edit it and restart to change them.

```
[server] sms-server 0.1.0, game 2026.10.20-beta.8cedf48
[server] Wrote server.ini with every setting explained. Edit it and restart the server to change them.
[server] Disc image: Super Mario Sunshine.iso
[server] Players reach UDP port 24002; listed publicly once the game is hosting.
[server] The game is hosting: opening the room.
[server] Room code K06-8K7: players join with it from their Multiplayer window, or from the public list.
[server] The server is in the public list: anyone can join it from their Multiplayer window.
```

Ctrl+C stops it, and the room server takes it out of the list at once.

**Linux** needs glibc 2.34 or newer (Ubuntu 22.04, Debian 12, Fedora 35, RHEL 9 or newer) and these libraries, which a desktop already has:

```sh
sudo apt install libsdl2-2.0-0 libegl1 libssl3     # Debian, Ubuntu
sudo dnf install SDL2 libglvnd-egl openssl-libs     # Fedora, RHEL
```

**Windows** needs 64-bit Windows; the DLLs it needs are in the zip.

### Game modes

A game mode is a mod in the `mods` folder, one folder each.
The [Squid Race](https://github.com/chasem-dev/sms-squid-race-server) is one: unzip its release into `mods/` (giving `mods/squid-race/`), restart, and the server runs Blooper races in Ricco Harbor for whoever joins.
With no mods, the server is a place to play together, starting in Delfino Plaza (or the `stage` you choose).

Mods whose `mod.ini` says `side = server` run on the server only: players see what they do (banners, timers, warps, Bloopers) with the plain game.
The [mod SDK's manual](https://github.com/chasem-dev/sms-mod-sdk/blob/main/docs/MODDING.md#server-mods) explains how to write one.

### How players join

- **The public list**: with `public = true`, the server is in every launcher's Multiplayer window, with its name, its status line ("Squid Race: waiting for players (1 of 2)") and how many are playing.
- **The room code**, printed when the server starts (it may change if the server loses the room server for a while; the new one is printed).
- **The address**: this computer's IP address and the port (`203.0.113.7:24002`), for players on the same network or when the port is forwarded.

Forwarding the UDP port on your router (or opening it in a VPS's firewall) lets every player connect directly.
Without it, players still join with the code or from the list: most connect directly anyway (hole punching), and the rest through the room server's relay, which adds a little delay.

### Versions

A player's game must be the server's build, or, with `versions = drift` (the default), any build of the same multiplayer protocol with the same Super Mario Eclipse setting.
`min_version` and `min_launcher` turn away games and launchers older than those you name.
The server's release names its game: `beta` follows SMS Launcher's beta, and `v2026.10.18.1` is the launcher's game 2026.10.18.1.

### Running it as a service (Linux)

```ini
# /etc/systemd/system/sms-server.service
[Unit]
Description=Super Mario Sunshine server
After=network-online.target
Wants=network-online.target

[Service]
User=sms
WorkingDirectory=/opt/sms-server
ExecStart=/opt/sms-server/sms-server
Restart=on-failure
MemoryMax=1G

[Install]
WantedBy=multi-user.target
```

`sudo systemctl enable --now sms-server`, and `journalctl -u sms-server -f` shows its output.

## server.ini

| Setting | Default | |
| --- | --- | --- |
| `name` | Super Mario Sunshine server | The name in the public list (at most 23 characters). |
| `port` | 24002 | The UDP port players reach. |
| `public` | true | In every launcher's list of public games; `false`: joined with the room code or the address only. |
| `room_code` | true | `false`: no room code and no listing, nothing sent to the room server; players join with the address only. |
| `max_players` | 8 | Players at once (2 to 8). |
| `disc` | | The disc image; empty: the one image in the server's folder. |
| `mods` | mods | The folder of mods. |
| `mod_settings` | mod-settings | Where mods keep their settings. |
| `saves` | save | The server's memory card. |
| `stage` | | Where players start, as `area,episode` (`2,0`: Bianco Hills, episode 1); empty: where the mods take them, else Delfino Plaza. |
| `versions` | drift | `exact`: players need this exact build; `drift`: any build of the same protocol. |
| `min_version` | | The oldest game version that may join (`2026.10.18`). |
| `min_launcher` | | The oldest launcher version that may join (`0.1.63`). |
| `frame_rate` | | 30, 60 or 120: the game's rate on the server; empty: the game's default. |
| `room_server` | (SMS Launcher's) | The room server, for testing one of your own. |

`--config=path/to/server.ini` (or `SMS_SERVER_CONFIG`) reads another file; relative paths in it start at its folder.
Any `SMS_*` variable the game reads, set in the environment, wins over `server.ini`.

## Building it

```sh
git clone --recursive https://github.com/chasem-dev/sms-server
cd sms-server
./build.sh        # port/build/linux-64-server/sms-server
```

It builds the game ([`port/`](https://github.com/chasem-dev/sms-pc-port), a submodule) with this repository's sources added, so the game's [build prerequisites](https://github.com/chasem-dev/sms-pc-port/blob/main/BUILD.md) apply, plus OpenSSL's headers on Linux (`libssl-dev`).
On Windows, build in an MSYS2 MINGW64 shell.
`tools/package.sh sms-server.zip` makes the zip the releases have.
The unit tests need no game: `cmake -S tests -B build/tests && cmake --build build/tests && build/tests/sms_server_tests`.

[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) explains what the server does and how the pieces fit.

## License

MIT, as the [mod SDK](https://github.com/chasem-dev/sms-mod-sdk). The game's code is [sms-pc-port](https://github.com/chasem-dev/sms-pc-port)'s; you need your own copy of the game.
