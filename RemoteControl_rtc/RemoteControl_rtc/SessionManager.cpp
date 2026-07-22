#include "pch.h"
#include "SessionManager.h"

#include "PeerSession.h"
#include "WebRtcEngine.h"

SessionManager::SessionManager(WebRtcEngine& engine) : engine_(engine) {}
SessionManager::~SessionManager() { CloseAll(); }

PeerSession* SessionManager::CreateSession(const std::string& remote_user_id, const MediaOptions& options)
{
	if (remote_user_id.empty() || FindSession(remote_user_id))
	{
		return nullptr;
	}

	auto session = std::make_unique<PeerSession>(engine_, remote_user_id, options);
	if (!session->Create())
	{
		return nullptr;
	}
	auto* result = session.get();
	sessions_.emplace(remote_user_id, std::move(session));
	return result;
}

PeerSession* SessionManager::FindSession(const std::string& remote_user_id) const
{
	auto it = sessions_.find(remote_user_id);
	return it == sessions_.end() ? nullptr : it->second.get();
}

void SessionManager::RemoveSession(const std::string& remote_user_id)
{
	auto it = sessions_.find(remote_user_id);
	if (it != sessions_.end())
	{
		it->second->Close();
		sessions_.erase(it);
	}
}

void SessionManager::CloseAll()
{
	for (auto& item : sessions_)
	{
		item.second->Close();
	}
	sessions_.clear();
}

void SessionManager::SetRemoteControlAuthorized(bool authorized)
{
	for (auto& item : sessions_) item.second->SetRemoteControlAuthorized(authorized);
}
