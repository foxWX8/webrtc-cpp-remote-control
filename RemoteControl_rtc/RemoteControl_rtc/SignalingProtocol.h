#pragma once

#include <string>
#include <vector>

// Small JSON helper for the fixed signaling envelope. It deliberately only
// exposes fields used by this client, rather than accepting arbitrary JSON.
class SignalingProtocol final
{
public:
	static std::string StringField(const std::string& json, const char* name);
	static int IntField(const std::string& json, const char* name, int fallback = 0);
	static bool BoolField(const std::string& json, const char* name, bool fallback = false);
	static std::vector<std::string> UserIds(const std::string& json);
	static std::string Escape(const std::string& value);
	static std::string Offer(const std::string& to, const std::string& sdp);
	static std::string Answer(const std::string& to, const std::string& sdp);
	static std::string Candidate(const std::string& to, const std::string& mid, int mline_index, const std::string& candidate);
	static std::string Hangup(const std::string& to);
};
