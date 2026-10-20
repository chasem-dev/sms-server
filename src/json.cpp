#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace srv {

const Json& Json::operator[](const char* key) const
{
	static const Json none;
	if (type == Object)
		for (const auto& m : members)
			if (m.first == key)
				return m.second;
	return none;
}

bool Json::isInt() const { return type == Number && std::floor(number) == number && std::fabs(number) < 9.0e15; }

namespace {

struct Parser {
	const char* p;
	const char* end;
	int depth = 0;

	void space()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;
	}

	bool literal(const char* word)
	{
		const size_t n = strlen(word);
		if ((size_t)(end - p) < n || memcmp(p, word, n) != 0)
			return false;
		p += n;
		return true;
	}

	static void utf8(std::string& out, unsigned c)
	{
		if (c < 0x80)
			out += char(c);
		else if (c < 0x800) {
			out += char(0xC0 | c >> 6);
			out += char(0x80 | (c & 63));
		} else if (c < 0x10000) {
			out += char(0xE0 | c >> 12);
			out += char(0x80 | ((c >> 6) & 63));
			out += char(0x80 | (c & 63));
		} else {
			out += char(0xF0 | c >> 18);
			out += char(0x80 | ((c >> 12) & 63));
			out += char(0x80 | ((c >> 6) & 63));
			out += char(0x80 | (c & 63));
		}
	}

	bool hex4(unsigned* v)
	{
		if (end - p < 4)
			return false;
		*v = 0;
		for (int i = 0; i < 4; i++) {
			const char c = *p++;
			*v <<= 4;
			if (c >= '0' && c <= '9')
				*v |= unsigned(c - '0');
			else if (c >= 'a' && c <= 'f')
				*v |= unsigned(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				*v |= unsigned(c - 'A' + 10);
			else
				return false;
		}
		return true;
	}

	bool string(std::string* out)
	{
		if (p >= end || *p != '"')
			return false;
		p++;
		while (p < end && *p != '"') {
			if ((unsigned char)*p < 0x20)
				return false;
			if (*p != '\\') {
				*out += *p++;
				continue;
			}
			if (++p >= end)
				return false;
			const char e = *p++;
			switch (e) {
			case '"': *out += '"'; break;
			case '\\': *out += '\\'; break;
			case '/': *out += '/'; break;
			case 'b': *out += '\b'; break;
			case 'f': *out += '\f'; break;
			case 'n': *out += '\n'; break;
			case 'r': *out += '\r'; break;
			case 't': *out += '\t'; break;
			case 'u': {
				unsigned c;
				if (!hex4(&c))
					return false;
				if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
					const char* back = p;
					p += 2;
					unsigned low;
					if (hex4(&low) && low >= 0xDC00 && low < 0xE000)
						c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
					else
						p = back;
				}
				utf8(*out, c);
				break;
			}
			default: return false;
			}
		}
		if (p >= end)
			return false;
		p++;
		return true;
	}

	bool value(Json* v)
	{
		if (++depth > 32)
			return false;
		space();
		if (p >= end)
			return false;
		bool ok = true;
		if (*p == '{') {
			v->type = Json::Object;
			p++;
			space();
			if (p < end && *p == '}')
				p++;
			else
				for (;;) {
					space();
					std::string key;
					Json member;
					if (!string(&key))
						return false;
					space();
					if (p >= end || *p++ != ':' || !value(&member))
						return false;
					v->members.emplace_back(std::move(key), std::move(member));
					space();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == '}') {
						p++;
						break;
					}
					return false;
				}
		} else if (*p == '[') {
			v->type = Json::Array;
			p++;
			space();
			if (p < end && *p == ']')
				p++;
			else
				for (;;) {
					Json item;
					if (!value(&item))
						return false;
					v->items.push_back(std::move(item));
					space();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == ']') {
						p++;
						break;
					}
					return false;
				}
		} else if (*p == '"') {
			v->type = Json::String;
			ok = string(&v->text);
		} else if (literal("true")) {
			v->type = Json::Bool;
			v->boolean = true;
		} else if (literal("false")) {
			v->type = Json::Bool;
		} else if (literal("null")) {
			v->type = Json::Null;
		} else {
			std::string number;
			while (p < end && strchr("+-0123456789.eE", *p))
				number += *p++;
			char* stop = nullptr;
			v->type = Json::Number;
			v->number = strtod(number.c_str(), &stop);
			ok = !number.empty() && stop && *stop == 0;
		}
		depth--;
		return ok;
	}
};

} // namespace

bool Json::parse(const std::string& input, Json* out)
{
	Parser parser{input.data(), input.data() + input.size()};
	*out = Json();
	if (!parser.value(out))
		return false;
	parser.space();
	return parser.p == parser.end;
}

std::string quote(const std::string& s)
{
	std::string out = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += char(c);
		} else if (c < 0x20) {
			char esc[8];
			snprintf(esc, sizeof esc, "\\u%04x", c);
			out += esc;
		} else
			out += char(c);
	}
	return out + "\"";
}

} // namespace srv
