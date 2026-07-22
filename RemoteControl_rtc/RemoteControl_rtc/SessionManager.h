#pragma once

#include <map>
#include <memory>
#include <string>

#include "MediaOptions.h"

class PeerSession;
class WebRtcEngine;

class SessionManager final
{
public:
	explicit SessionManager(WebRtcEngine& engine);
	~SessionManager();

	PeerSession* CreateSession(const std::string& remote_user_id, const MediaOptions& options);
	PeerSession* FindSession(const std::string& remote_user_id) const;
	void RemoveSession(const std::string& remote_user_id);
	void CloseAll();
	void SetRemoteControlAuthorized(bool authorized);
	size_t SessionCount() const { return sessions_.size(); }

private:
	WebRtcEngine& engine_;
	std::map<std::string, std::unique_ptr<PeerSession>> sessions_;
};
