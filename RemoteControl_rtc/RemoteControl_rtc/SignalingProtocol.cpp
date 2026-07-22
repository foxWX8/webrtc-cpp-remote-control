#include "pch.h"
#include "SignalingProtocol.h"

namespace
{
size_t FindValue(const std::string& json, const char* name)
{
	const std::string key = std::string("\"") + name + "\"";
	size_t position = json.find(key);
	if (position == std::string::npos) return position;
	position = json.find(':', position + key.size());
	if (position == std::string::npos) return position;
	return json.find_first_not_of(" \t\r\n", position + 1);
}
}

std::string SignalingProtocol::StringField(const std::string& json, const char* name)
{
	size_t position = FindValue(json, name);
	if (position == std::string::npos || json[position] != '"') return {};
	std::string result;
	for (++position; position < json.size(); ++position)
	{
		if (json[position] == '"') return result;
		if (json[position] == '\\' && ++position < json.size())
		{
			switch (json[position]) { case 'n': result += '\n'; break; case 'r': result += '\r'; break; case 't': result += '\t'; break; default: result += json[position]; break; }
		}
		else result += json[position];
	}
	return {};
}

int SignalingProtocol::IntField(const std::string& json, const char* name, int fallback)
{
	const size_t position = FindValue(json, name);
	if (position == std::string::npos) return fallback;
	char* end = nullptr;
	const long value = strtol(json.c_str() + position, &end, 10);
	return end == json.c_str() + position ? fallback : static_cast<int>(value);
}

bool SignalingProtocol::BoolField(const std::string& json, const char* name, bool fallback)
{
	const size_t position = FindValue(json, name);
	if (position == std::string::npos) return fallback;
	if (json.compare(position, 4, "true") == 0) return true;
	if (json.compare(position, 5, "false") == 0) return false;
	return fallback;
}

std::vector<std::string> SignalingProtocol::UserIds(const std::string& json)
{
	std::vector<std::string> users;
	size_t position = 0;
	while ((position = json.find("\"userId\"", position)) != std::string::npos)
	{
		const std::string id = StringField(json.substr(position), "userId");
		if (!id.empty()) users.push_back(id);
		position += 8;
	}
	return users;
}

std::string SignalingProtocol::Escape(const std::string& value)
{
	std::string result;
	for (char ch : value)
	{
		switch (ch) { case '\\': result += "\\\\"; break; case '"': result += "\\\""; break; case '\n': result += "\\n"; break; case '\r': result += "\\r"; break; default: result += ch; break; }
	}
	return result;
}

std::string SignalingProtocol::Offer(const std::string& to, const std::string& sdp)
{
	return "{\"type\":\"offer\",\"toUserId\":\"" + Escape(to) + "\",\"sdp\":\"" + Escape(sdp) + "\"}";
}

std::string SignalingProtocol::Answer(const std::string& to, const std::string& sdp)
{
	return "{\"type\":\"answer\",\"toUserId\":\"" + Escape(to) + "\",\"sdp\":\"" + Escape(sdp) + "\"}";
}

std::string SignalingProtocol::Candidate(const std::string& to, const std::string& mid, int mline_index, const std::string& candidate)
{
	return "{\"type\":\"ice-candidate\",\"toUserId\":\"" + Escape(to) + "\",\"mid\":\"" + Escape(mid) + "\",\"mlineIndex\":" + std::to_string(mline_index) + ",\"candidate\":\"" + Escape(candidate) + "\"}";
}

std::string SignalingProtocol::Hangup(const std::string& to)
{
	return "{\"type\":\"hangup\",\"toUserId\":\"" + Escape(to) + "\"}";
}
