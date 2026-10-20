// sms-server: Super Mario Sunshine (sms-pc-port) as a dedicated server.
//
// The game calls sms_server_init first thing (platform/port_runtime.cpp,
// built in with -DSMS_SERVER_DIR). It reads server.ini, turns it into the
// environment the game reads (a headless dedicated multiplayer host that
// draws nothing and plays no sound), and opens the room that lists the
// server in every launcher's public games. Then the game starts as usual.

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "config.h"
#include "room.h"

#ifndef SMS_SERVER_VERSION
#define SMS_SERVER_VERSION "dev"
#endif
#ifndef SMS_SERVER_GAME_VERSION
#define SMS_SERVER_GAME_VERSION ""
#endif

namespace {

void say(const std::string& text)
{
	fprintf(stderr, "[server] %s\n", text.c_str());
	fflush(stderr);
}

void setEnv(const char* name, const std::string& value, bool overwrite = false)
{
	if (!overwrite && getenv(name))
		return;
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

void makeDir(const std::string& path)
{
	if (path.empty())
		return;
#ifdef _WIN32
	_mkdir(path.c_str());
#else
	mkdir(path.c_str(), 0755);
#endif
}

bool hasSuffix(const std::string& s, const char* suffix)
{
	const size_t n = strlen(suffix);
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)s[s.size() - n + i]) != suffix[i])
			return false;
	return true;
}

// The disc images in `dir`.
std::vector<std::string> discImages(const std::string& dir)
{
	std::vector<std::string> found;
#ifdef _WIN32
	_finddata_t data;
	intptr_t h = _findfirst((dir + "/*").c_str(), &data);
	if (h == -1)
		return found;
	do {
		const std::string name = data.name;
#else
	DIR* d = opendir(dir.c_str());
	if (!d)
		return found;
	while (dirent* entry = readdir(d)) {
		const std::string name = entry->d_name;
#endif
		if (hasSuffix(name, ".iso") || hasSuffix(name, ".gcm") || hasSuffix(name, ".ciso"))
			found.push_back(dir == "." ? name : dir + "/" + name);
#ifdef _WIN32
	} while (_findnext(h, &data) == 0);
	_findclose(h);
#else
	}
	closedir(d);
#endif
	return found;
}

srv::Room g_room;
std::atomic<int> g_stop{0};

void onSignal(int)
{
	if (g_stop.fetch_add(1) >= 1)
		_exit(130); // a second Ctrl+C: at once
}

#ifdef _WIN32
BOOL WINAPI onConsole(DWORD)
{
	onSignal(0);
	return TRUE;
}
#endif

// Ctrl+C (or a service manager's SIGTERM): close the room, so the room server
// takes the server out of the list at once, and stop.
void watchForStop()
{
	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);
#ifdef _WIN32
	SetConsoleCtrlHandler(onConsole, TRUE);
#endif
	std::thread([] {
		while (!g_stop)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		say("Stopping.");
		g_room.stop();
		fflush(stdout);
		fflush(stderr);
		_exit(0);
	}).detach();
}

} // namespace

extern "C" void sms_server_init(int argc, char** argv)
{
	say(std::string("sms-server " SMS_SERVER_VERSION) + (*SMS_SERVER_GAME_VERSION ? ", game " SMS_SERVER_GAME_VERSION : ""));
	std::string configPath = getenv("SMS_SERVER_CONFIG") ? getenv("SMS_SERVER_CONFIG") : "server.ini";
	bool discArgument = false;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--config=", 9) == 0)
			configPath = argv[i] + 9;
		else if (argv[i][0] != '-')
			discArgument = true; // the game reads it as its disc image
	}

	srv::Config config;
	bool created = false;
	if (!srv::loadConfig(configPath, &config, &created)) {
		say("Couldn't read or write " + configPath + ".");
		exit(1);
	}
	if (created)
		say("Wrote " + configPath + " with every setting explained. Edit it and restart the server to change them.");
	else
		say("Settings from " + configPath + ".");
	for (const std::string& problem : config.problems)
		say(configPath + ": " + problem);

	// the disc image
	if (!discArgument && !getenv("SMS_DISC_ROOT")) {
		std::string disc = config.pathOf("disc");
		if (disc.empty()) {
			const std::vector<std::string> found = discImages(config.dir);
			const std::string where = config.dir == "." ? "this folder" : config.dir;
			if (found.size() == 1)
				disc = found[0];
			else {
				say(found.empty() ? "No disc image: put your Super Mario Sunshine (USA) image (.iso, .gcm or .ciso) in " +
				                        where + ", or name it with disc = in " + configPath + "."
				                  : "Several disc images in " + where + ": name the one to use with disc = in " + configPath + ".");
				exit(1);
			}
		}
		setEnv("SMS_DISC_ROOT", disc, true);
		say("Disc image: " + disc);
	}

	// a game that draws nothing and plays no sound, hosting from the start
	setEnv("SMS_HEADLESS", "1");
	setEnv("SMS_GX_BACKEND", "none");
	setEnv("SMS_AUDIO", "0");
	setEnv("SMS_TEXTURE_PACKS", "0");
	setEnv("SMS_SKIP_MOVIES", "1");
	setEnv("SMS_SKIP_INTRO", "1");
	setEnv("SMS_QUIET_STUBS", "1");
	setEnv("SMS_SETTINGS", ""); // no settings.txt: the server's own
	if (*SMS_SERVER_GAME_VERSION)
		setEnv("SMS_GAME_VERSION", SMS_SERVER_GAME_VERSION);
	setEnv("SMS_MULTIPLAYER", "host");
	setEnv("SMS_MULTIPLAYER_DEDICATED", "1");

	const std::string mods = config.pathOf("mods"), modSettings = config.pathOf("mod_settings"), saves = config.pathOf("saves");
	makeDir(mods);
	makeDir(modSettings);
	makeDir(saves);
	setEnv("SMS_NATIVE_MODS_DIR", mods);
	setEnv("SMS_NATIVE_MODS_DATA", modSettings);
	setEnv("SMS_SAVE_DIR", saves);

	std::string name = config.get("name");
	if (name.size() > 23)
		name.resize(23);
	setEnv("SMS_MULTIPLAYER_NAME", name);
	const int players = config.number("max_players");
	if (players >= 2 && players <= 8)
		setEnv("SMS_MULTIPLAYER_MAX_PLAYERS", std::to_string(players));
	if (!config.get("stage").empty())
		setEnv("SMS_MULTIPLAYER_STAGE", config.get("stage"));
	setEnv("SMS_MULTIPLAYER_VERSIONS", config.get("versions"));
	if (!config.get("min_version").empty())
		setEnv("SMS_MULTIPLAYER_MIN_VERSION", config.get("min_version"));
	if (!config.get("min_launcher").empty())
		setEnv("SMS_MULTIPLAYER_MIN_LAUNCHER", config.get("min_launcher"));
	if (!config.get("frame_rate").empty())
		setEnv("SMS_FRAME_RATE", config.get("frame_rate"));

	const int port = config.number("port");
	if (port < 1 || port > 65535) {
		say(configPath + ": port must be 1 to 65535.");
		exit(1);
	}
	watchForStop();
	if (!config.flag("room_code")) {
		// the game listens on the port itself
		setEnv("SMS_MULTIPLAYER_PORT", std::to_string(port), true);
		say("No room code (room_code = false): players join with this computer's address and port " +
		    std::to_string(port) + ".");
		return;
	}

	curl_global_init(CURL_GLOBAL_DEFAULT);
	srv::RoomOptions options;
	options.port = (uint16_t)port;
	options.name = name;
	options.isPublic = config.flag("public");
	options.url = config.get("room_server");
	while (!options.url.empty() && options.url.back() == '/')
		options.url.pop_back();
	options.log = [](const std::string& text) { say(text); };
	std::string error;
	if (!g_room.start(options, &error)) {
		say(error);
		exit(1);
	}
	// the game listens on this computer only, behind the room
	setEnv("SMS_MULTIPLAYER_PORT", std::to_string(g_room.gamePort()), true);
	setEnv("SMS_MULTIPLAYER_LOCAL", "1", true);
	say("Players reach UDP port " + std::to_string(port) + (options.isPublic ? "; listed publicly once the game is hosting." : "; not listed (public = false)."));
}
