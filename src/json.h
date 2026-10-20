// Just enough JSON for the room server's messages: parsing small objects,
// and writing them by hand with quote().
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace srv {

struct Json {
	enum Type { Null, Bool, Number, String, Array, Object } type = Null;
	bool boolean = false;
	double number = 0;
	std::string text;
	std::vector<Json> items;                           // Array
	std::vector<std::pair<std::string, Json>> members; // Object

	// The member `key` of an object (a Null for anything else).
	const Json& operator[](const char* key) const;
	bool isInt() const;
	long long integer() const { return (long long)number; }

	// The value `input` holds, or false when it is not JSON.
	static bool parse(const std::string& input, Json* out);
};

// `s` as a JSON string, quotes included.
std::string quote(const std::string& s);

} // namespace srv
