#include "config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace srv {

const std::vector<Setting>& settings()
{
	static const std::vector<Setting> list = {
	    {"name", "Super Mario Sunshine server", "The server's name in the launcher's list of public games (at most 23 characters)."},
	    {"port", "24002", "The UDP port players reach. Forward it on your router for direct connections;\n"
	                      "without that, players behind other routers still join, some through the relay."},
	    {"public", "true", "true: in every launcher's list of public games. false: players join with the\n"
	                       "room code (printed at start) or the address only."},
	    {"room_code", "true", "false: no room code and no listing; nothing is sent to the room server, and\n"
	                          "players join with the address and port only."},
	    {"max_players", "8", "Players at once (2 to 8)."},
	    {"disc", "", "Your Super Mario Sunshine (USA, GMSE01) disc image: .iso, .gcm or .ciso. Empty: the\n"
	                 "one image in this folder."},
	    {"mods", "mods", "The folder of mods, one folder each (a game mode such as squid-race/). Mods that\n"
	                     "say side = server change the game for everyone, with no download for players."},
	    {"mod_settings", "mod-settings", "Where mods keep their settings."},
	    {"saves", "save", "The server's memory card (save files)."},
	    {"stage", "", "The stage players start in, as \"area,episode\" (2,0: Bianco Hills episode 1).\n"
	                  "Empty: where the mods take them, else Delfino Plaza."},
	    {"versions", "drift", "exact: players need this server's exact game build. drift: other builds of the\n"
	                          "same multiplayer protocol may join too (with the same Eclipse setting)."},
	    {"min_version", "", "The oldest game version that may join (e.g. 2026.10.18), or empty."},
	    {"min_launcher", "", "The oldest launcher version that may join (e.g. 0.1.63), or empty."},
	    {"frame_rate", "", "30, 60 or 120: the game's rate on the server. Empty: the game's default."},
	    {"room_server", "https://sms-launcher-telemetry.chasemdev.workers.dev", "The room server (for testing a room server of your own)."},
	};
	return list;
}

std::string defaultConfigText()
{
	std::string text =
	    "; Super Mario Sunshine dedicated server (sms-server) settings.\n"
	    "; \"key = value\" lines; ';' or '#' starts a comment. Changes take effect when the\n"
	    "; server restarts. Environment variables the game reads (SMS_*) win over these.\n";
	for (const Setting& s : settings()) {
		text += "\n";
		std::string help = s.help;
		size_t start = 0;
		for (;;) {
			const size_t end = help.find('\n', start);
			text += "; " + help.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
		text += std::string(s.key) + " = " + s.fallback + "\n";
	}
	return text;
}

static std::string trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a]))
		a++;
	while (b > a && isspace((unsigned char)s[b - 1]))
		b--;
	return s.substr(a, b - a);
}

bool parseConfig(const std::string& text, Config* config)
{
	size_t start = 0;
	int line = 0;
	while (start <= text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string s = text.substr(start, end - start);
		start = end + 1;
		line++;
		if (line == 1 && s.compare(0, 3, "\xEF\xBB\xBF") == 0)
			s = s.substr(3); // a byte-order mark from Notepad
		s = trim(s);
		if (s.empty() || s[0] == ';' || s[0] == '#')
			continue;
		const size_t eq = s.find('=');
		if (eq == std::string::npos) {
			config->problems.push_back("line " + std::to_string(line) + " is not \"key = value\": " + s);
			continue;
		}
		std::string key = trim(s.substr(0, eq)), value = trim(s.substr(eq + 1));
		for (char& c : key)
			c = char(tolower((unsigned char)c));
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);
		bool known = false;
		for (const Setting& setting : settings())
			known = known || key == setting.key;
		if (!known)
			config->problems.push_back("line " + std::to_string(line) + ": unknown setting \"" + key + "\"");
		config->values[key] = value;
		if (end == text.size())
			break;
	}
	return true;
}

bool loadConfig(const std::string& path, Config* config, bool* created)
{
	*created = false;
	config->path = path;
	const size_t slash = path.find_last_of("/\\");
	config->dir = slash == std::string::npos ? "." : path.substr(0, slash);
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) {
		f = fopen(path.c_str(), "wb");
		if (!f)
			return false;
		const std::string text = defaultConfigText();
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
		*created = true;
		return parseConfig(text, config);
	}
	std::string text;
	char buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
		text.append(buffer, n);
	fclose(f);
	return parseConfig(text, config);
}

std::string Config::get(const std::string& key) const
{
	auto it = values.find(key);
	if (it != values.end())
		return it->second;
	for (const Setting& s : settings())
		if (key == s.key)
			return s.fallback;
	return std::string();
}

bool Config::flag(const std::string& key) const
{
	std::string v = get(key);
	for (char& c : v)
		c = char(tolower((unsigned char)c));
	return v == "true" || v == "yes" || v == "on" || v == "1";
}

int Config::number(const std::string& key) const { return atoi(get(key).c_str()); }

std::string Config::pathOf(const std::string& key) const
{
	const std::string v = get(key);
	if (v.empty())
		return v;
	const bool absolute = v[0] == '/' || v[0] == '\\' || (v.size() > 2 && v[1] == ':');
	return absolute || dir == "." ? v : dir + "/" + v;
}

} // namespace srv
