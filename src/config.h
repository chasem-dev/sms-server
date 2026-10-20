// server.ini: "key = value" lines, ';' or '#' comments, like a Minecraft
// server.properties. Written with every setting explained the first time the
// server starts without one.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace srv {

struct Config {
	std::string path;                         // the file read (or written)
	std::string dir;                          // its folder: relative paths in it start there
	std::map<std::string, std::string> values; // as written, keys in lower case
	std::vector<std::string> problems;        // lines that weren't "key = value", unknown keys

	std::string get(const std::string& key) const; // the value, or the default
	bool flag(const std::string& key) const;       // true/yes/on/1
	int number(const std::string& key) const;
	std::string pathOf(const std::string& key) const; // relative to dir; '' stays ''
};

// Reads `path`. A missing file is written from the defaults first (`created`
// says so); false when it can be neither read nor written.
bool loadConfig(const std::string& path, Config* config, bool* created);
bool parseConfig(const std::string& text, Config* config);

// The settings, their defaults and what each does, in the order written.
struct Setting {
	const char* key;
	const char* fallback;
	const char* help;
};
const std::vector<Setting>& settings();
std::string defaultConfigText();

} // namespace srv
