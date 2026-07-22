#include "pch.h"
#include "PeerSession.h"

#include "WebRtcEngine.h"
#include "RemoteVideoRenderer.h"
#include "RemoteControlChannel.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <unordered_map>

namespace
{
	constexpr const char* kDefaultStunUri = "stun:150.158.3.4:3478";
	constexpr const char* kDefaultTurnUdpUri = "turn:150.158.3.4:3478?transport=udp";
	constexpr const char* kDefaultTurnTcpUri = "turn:150.158.3.4:3478?transport=tcp";
	// Credentials must never be committed. Supply both values through the
	// REMOTE_CONTROL_TURN_USERNAME and REMOTE_CONTROL_TURN_PASSWORD environment
	// variables. STUN remains available when TURN credentials are not configured.
	constexpr const char* kDefaultTurnUsername = "";
	constexpr const char* kDefaultTurnPassword = "";

	std::string EnvironmentOrDefault(const char* name, const char* fallback)
	{
		char* value = nullptr;
		size_t length = 0;
#ifdef _WIN32
		if (_dupenv_s(&value, &length, name) == 0 && value)
		{
			std::string result(value);
			std::free(value);
			if (!result.empty()) return result;
		}
		// A process launched by an already-running Explorer may not inherit a
		// newly provisioned user environment value. Read the persistent HKCU
		// value as a fallback so TURN provisioning works without logging out.
		DWORD bytes = 0;
		if (::RegGetValueA(HKEY_CURRENT_USER, "Environment", name, RRF_RT_REG_SZ,
			nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > 1)
		{
			std::vector<char> registry_value(bytes, '\0');
			if (::RegGetValueA(HKEY_CURRENT_USER, "Environment", name, RRF_RT_REG_SZ,
				nullptr, registry_value.data(), &bytes) == ERROR_SUCCESS && registry_value[0])
				return std::string(registry_value.data());
		}
#else
		if (const char* environment = std::getenv(name); environment && *environment)
			return environment;
#endif
		return fallback;
	}

	void ConfigureIceServers(libwebrtc::RTCConfiguration& configuration)
	{
		const std::string stun_uri = EnvironmentOrDefault(
			"REMOTE_CONTROL_STUN_URI", kDefaultStunUri);
		const std::string turn_udp_uri = EnvironmentOrDefault(
			"REMOTE_CONTROL_TURN_UDP_URI", kDefaultTurnUdpUri);
		const std::string turn_tcp_uri = EnvironmentOrDefault(
			"REMOTE_CONTROL_TURN_TCP_URI", kDefaultTurnTcpUri);
		const std::string turns_uri = EnvironmentOrDefault("REMOTE_CONTROL_TURNS_URI", "");
		const std::string username = EnvironmentOrDefault("REMOTE_CONTROL_TURN_USERNAME", kDefaultTurnUsername);
		const std::string password = EnvironmentOrDefault("REMOTE_CONTROL_TURN_PASSWORD", kDefaultTurnPassword);

		configuration.ice_servers[0].uri = stun_uri;
		// This wrapper crashes in CreateDataChannel when a TURN URI is present
		// with empty long-term credentials. Keep STUN active, and add TURN only
		// when both values have explicitly been supplied.
		if (!username.empty() && !password.empty())
		{
			for (int index = 1; index <= 2; ++index)
			{
				configuration.ice_servers[index].username = username;
				configuration.ice_servers[index].password = password;
			}
			configuration.ice_servers[1].uri = turn_udp_uri;
			configuration.ice_servers[2].uri = turn_tcp_uri;
			if (!turns_uri.empty())
			{
				configuration.ice_servers[3].username = username;
				configuration.ice_servers[3].password = password;
				configuration.ice_servers[3].uri = turns_uri;
			}
		}
		configuration.type = libwebrtc::IceTransportsType::kAll;
		configuration.ice_candidate_pool_size = 2;
	}

	double NumericMember(const libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>& report,
		const char* name, bool* found = nullptr)
	{
		if (found) *found = false;
		if (!report) return 0.0;
		const auto members = report->Members();
		for (size_t index = 0; index < members.size(); ++index)
		{
			const auto& member = members[index];
			if (!member || !member->IsDefined() || member->GetName().std_string() != name) continue;
			if (found) *found = true;
			switch (member->GetType())
			{
			case libwebrtc::RTCStatsMember::kInt32: return member->ValueInt32();
			case libwebrtc::RTCStatsMember::kUint32: return member->ValueUint32();
			case libwebrtc::RTCStatsMember::kInt64: return static_cast<double>(member->ValueInt64());
			case libwebrtc::RTCStatsMember::kUint64: return static_cast<double>(member->ValueUint64());
			case libwebrtc::RTCStatsMember::kDouble: return member->ValueDouble();
			default:
				if (found) *found = false;
				return 0.0;
			}
		}
		return 0.0;
	}

	double NumericMemberFallback(const libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>& report,
		const char* primary_name, const char* fallback_name, bool* found = nullptr)
	{
		bool primary_found = false;
		const double primary = NumericMember(report, primary_name, &primary_found);
		if (primary_found)
		{
			if (found) *found = true;
			return primary;
		}
		return NumericMember(report, fallback_name, found);
	}

	uint64_t UnsignedMember(const libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>& report,
		const char* name, bool* found = nullptr)
	{
		bool member_found = false;
		const double value = NumericMember(report, name, &member_found);
		if (found) *found = member_found;
		return member_found && value > 0.0 ? static_cast<uint64_t>(value) : 0;
	}

	int64_t SignedMember(const libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>& report,
		const char* name, bool* found = nullptr)
	{
		bool member_found = false;
		const double value = NumericMember(report, name, &member_found);
		if (found) *found = member_found;
		return member_found ? static_cast<int64_t>(value) : 0;
	}

	std::string StringMember(const libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>& report,
		const char* name)
	{
		if (!report) return {};
		const auto members = report->Members();
		for (size_t index = 0; index < members.size(); ++index)
		{
			const auto& member = members[index];
			if (member && member->IsDefined() && member->GetName().std_string() == name &&
				member->GetType() == libwebrtc::RTCStatsMember::kString)
				return member->ValueString().std_string();
		}
		return {};
	}

	bool BoolMember(const libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>& report,
		const char* name)
	{
		if (!report) return false;
		const auto members = report->Members();
		for (size_t index = 0; index < members.size(); ++index)
		{
			const auto& member = members[index];
			if (member && member->IsDefined() && member->GetName().std_string() == name &&
				member->GetType() == libwebrtc::RTCStatsMember::kBool)
				return member->ValueBool();
		}
		return false;
	}

	struct CandidateInfo
	{
		std::string type;
		std::string protocol;
	};

	PeerConnectionStats CollectPeerConnectionStats(
		const libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>>& reports)
	{
		PeerConnectionStats result;
		result.sample_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();

		std::unordered_map<std::string, CandidateInfo> candidates;
		std::string selected_pair_id;
		for (size_t report_index = 0; report_index < reports.size(); ++report_index)
		{
			const auto& report = reports[report_index];
			if (!report) continue;
			const std::string type = report->type().std_string();
			if (type == "transport")
			{
				const std::string id = StringMember(report, "selectedCandidatePairId");
				if (!id.empty()) selected_pair_id = id;
			}
			else if (type == "local-candidate" || type == "remote-candidate")
			{
				candidates.emplace(report->id().std_string(), CandidateInfo{
					StringMember(report, "candidateType"), StringMember(report, "protocol") });
			}
		}

		libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats> selected_pair;
		int selected_pair_score = -1;
		for (size_t report_index = 0; report_index < reports.size(); ++report_index)
		{
			const auto& report = reports[report_index];
			if (!report) continue;
			const std::string type = report->type().std_string();
			if (type == "inbound-rtp" || type == "ssrc")
			{
				std::string kind = StringMember(report, "kind");
				if (kind.empty()) kind = StringMember(report, "mediaType");
				bool has_bytes = false;
				bool has_packets = false;
				bool has_lost = false;
				const uint64_t bytes = UnsignedMember(report, "bytesReceived", &has_bytes);
				const uint64_t packets = UnsignedMember(report, "packetsReceived", &has_packets);
				const int64_t lost = SignedMember(report, "packetsLost", &has_lost);
				bool has_jitter = false;
				double jitter_ms = NumericMember(report, "jitter", &has_jitter) * 1000.0;
				if (!has_jitter) jitter_ms = NumericMember(report, "googJitterReceived", &has_jitter);
				if (kind == "video")
				{
					result.video_stats_available = result.video_stats_available || has_bytes || has_packets || has_lost;
					result.video_bytes_received += bytes;
					result.video_packets_received += packets;
					result.video_packets_lost += lost;
					result.video_frames_decoded += UnsignedMember(report, "framesDecoded");
					result.video_frames_per_second = NumericMemberFallback(report,
						"framesPerSecond", "googFrameRateReceived");
					result.video_width = static_cast<int>(NumericMemberFallback(report,
						"frameWidth", "googFrameWidthReceived"));
					result.video_height = static_cast<int>(NumericMemberFallback(report,
						"frameHeight", "googFrameHeightReceived"));
					if (has_jitter)
					{
						result.video_jitter_available = true;
						result.video_jitter_ms = jitter_ms;
					}
					bool has_buffer_delay = false;
					const double buffer_delay = NumericMember(report, "jitterBufferDelay", &has_buffer_delay);
					bool has_buffer_count = false;
					const uint64_t buffer_count = UnsignedMember(report, "jitterBufferEmittedCount", &has_buffer_count);
					if (has_buffer_delay && has_buffer_count)
					{
						result.video_buffer_available = true;
						result.video_jitter_buffer_delay_seconds += buffer_delay;
						result.video_jitter_buffer_emitted_count += buffer_count;
					}
					else
					{
						bool has_legacy_delay = false;
						result.video_playout_delay_ms = NumericMember(report, "googCurrentDelayMs", &has_legacy_delay);
						result.video_buffer_available = has_legacy_delay;
					}
				}
				else if (kind == "audio")
				{
					result.audio_stats_available = result.audio_stats_available || has_bytes || has_packets || has_lost;
					result.audio_bytes_received += bytes;
					result.audio_packets_received += packets;
					result.audio_packets_lost += lost;
					if (has_jitter)
					{
						result.audio_jitter_available = true;
						result.audio_jitter_ms = jitter_ms;
					}
				}
			}
			else if (type == "remote-inbound-rtp" && result.round_trip_time_ms <= 0.0)
			{
				bool has_round_trip_time = false;
				const double round_trip_time = NumericMember(report, "roundTripTime", &has_round_trip_time);
				if (has_round_trip_time)
				{
					result.round_trip_time_available = true;
					result.round_trip_time_ms = round_trip_time * 1000.0;
				}
			}
			else if (type == "candidate-pair" || type == "googCandidatePair")
			{
				int score = -1;
				if (!selected_pair_id.empty() && report->id().std_string() == selected_pair_id) score = 100;
				else if (BoolMember(report, "selected")) score = 80;
				else if (StringMember(report, "googActiveConnection") == "true") score = 80;
				else if (BoolMember(report, "nominated") && StringMember(report, "state") == "succeeded") score = 60;
				else if (StringMember(report, "state") == "succeeded") score = 20;
				if (score > selected_pair_score)
				{
					selected_pair_score = score;
					selected_pair = report;
				}
			}
		}

		if (selected_pair)
		{
			bool has_round_trip_time = false;
			const double round_trip_time = NumericMember(selected_pair, "currentRoundTripTime", &has_round_trip_time);
			if (has_round_trip_time)
			{
				result.round_trip_time_available = true;
				result.round_trip_time_ms = round_trip_time * 1000.0;
			}
			else
			{
				result.round_trip_time_ms = NumericMember(selected_pair, "googRtt", &has_round_trip_time);
				result.round_trip_time_available = has_round_trip_time;
			}
			result.available_incoming_bitrate_bps = NumericMemberFallback(selected_pair,
				"availableIncomingBitrate", "googAvailableReceiveBandwidth",
				&result.available_incoming_bitrate_available);
			const auto local = candidates.find(StringMember(selected_pair, "localCandidateId"));
			const auto remote = candidates.find(StringMember(selected_pair, "remoteCandidateId"));
			const std::string local_type = local == candidates.end() ? "" : local->second.type;
			const std::string remote_type = remote == candidates.end() ? "" : remote->second.type;
			std::string protocol = local == candidates.end() ? "" : local->second.protocol;
			if (protocol.empty() && remote != candidates.end()) protocol = remote->second.protocol;
			if (local_type == "relay" || remote_type == "relay") result.ice_route = "turn-relay";
			else if (local_type == "srflx" || local_type == "prflx" ||
				remote_type == "srflx" || remote_type == "prflx") result.ice_route = "stun-direct";
			else result.ice_route = "host-direct";
			if (!protocol.empty()) result.ice_route += "/" + protocol;
		}
		return result;
	}
}

struct PeerSession::StatsRequestState
{
	std::atomic<bool> alive{ true };
	std::atomic<bool> pending{ false };
};

PeerSession::PeerSession(WebRtcEngine& engine, std::string remote_user_id, MediaOptions options)
	: engine_(engine), remote_user_id_(std::move(remote_user_id)), options_(options),
	stats_request_state_(std::make_shared<StatsRequestState>())
{
}

PeerSession::~PeerSession()
{
	Close();
}

bool PeerSession::Create()
{
	stats_request_state_->alive = true;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		closed_ = false;
	}
	auto factory = engine_.Factory();
	if (!factory)
	{
		return false;
	}

	libwebrtc::RTCConfiguration configuration;
	configuration.offer_to_receive_audio = options_.receive_audio;
	configuration.offer_to_receive_video = options_.receive_video;
	configuration.local_audio_bandwidth = 128;
	configuration.local_video_bandwidth = 20000;
	configuration.screencast_min_bitrate = 2000;
	ConfigureIceServers(configuration);
	peer_connection_ = factory->Create(configuration, libwebrtc::RTCMediaConstraints::Create());
	if (!peer_connection_)
	{
		UpdateState(PeerSessionState::Failed);
		return false;
	}

	peer_connection_->RegisterRTCPeerConnectionObserver(this);
	return true;
}

bool PeerSession::CreateControlChannel()
{
	if (!peer_connection_ || !options_.enable_remote_control) return false;
	if (control_channel_) return true;
	libwebrtc::RTCDataChannelInit init;
	init.ordered = true;
	init.reliable = true;
	auto channel = peer_connection_->CreateDataChannel("remote-control", &init);
	if (!channel) return false;
	control_channel_ = std::make_unique<RemoteControlChannel>();
	control_channel_->SetEventCallback([this](const std::string& detail) { NotifyControlEvent(detail); });
	control_channel_->SetAuthorized(remote_control_authorized_.load());
	control_channel_->Attach(channel);
	return true;
}

bool PeerSession::AddLocalVideoTrack(libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track)
{
	if (!peer_connection_ || !track) return false;
	std::vector<libwebrtc::string> stream_ids;
	stream_ids.emplace_back("desktop-stream");
	const libwebrtc::vector<libwebrtc::string> ids(stream_ids);
	auto sender = peer_connection_->AddTrack(track, ids);
	if (!sender) return false;
	PreferCodec(libwebrtc::RTCMediaType::VIDEO, "video/H264");

	// Desktop text becomes unreadable if WebRTC adapts by reducing resolution.
	// Preserve native resolution and let congestion control reduce frame rate.
	auto parameters = sender->parameters();
	if (parameters)
	{
		parameters->SetDegradationPreference(libwebrtc::RTCDegradationPreference::MAINTAIN_RESOLUTION);
		auto encodings = parameters->encodings();
		for (size_t index = 0; index < encodings.size(); ++index)
		{
			const auto& encoding = encodings[index];
			if (!encoding) continue;
			encoding->set_scale_resolution_down_by(1.0);
			encoding->set_max_framerate(30.0);
			encoding->set_min_bitrate_bps(2500000);
			encoding->set_max_bitrate_bps(20000000);
			encoding->set_bitrate_priority(2.0);
			encoding->set_network_priority(libwebrtc::RTCPriority::kHigh);
		}
		parameters->set_encodings(encodings);
		sender->set_parameters(parameters);
	}
	return true;
}

bool PeerSession::AddLocalAudioTrack(libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> track)
{
	if (!peer_connection_ || !track) return false;
	std::vector<libwebrtc::string> stream_ids;
	stream_ids.emplace_back("audio-stream");
	const libwebrtc::vector<libwebrtc::string> ids(stream_ids);
	auto sender = peer_connection_->AddTrack(track, ids);
	if (!sender) return false;
	PreferCodec(libwebrtc::RTCMediaType::AUDIO, "audio/opus");
	return true;
}

void PeerSession::Close()
{
	stats_request_state_->alive = false;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		closed_ = true;
		pending_sdp_callback_ = {};
		ice_candidate_callback_ = {};
		media_event_callback_ = {};
		control_event_callback_ = {};
		pending_local_candidates_.clear();
		pending_remote_candidates_.clear();
	}
	if (!peer_connection_)
	{
		return;
	}

	if (remote_video_track_ && remote_video_renderer_) remote_video_track_->RemoveRenderer(remote_video_renderer_.get());
	remote_video_track_ = nullptr;
	remote_video_renderer_.reset();
	control_channel_.reset();
	peer_connection_->DeRegisterRTCPeerConnectionObserver();
	peer_connection_->Close();
	peer_connection_ = nullptr;
	UpdateState(PeerSessionState::Closed);
}

std::string PeerSession::IceServiceSummary()
{
	const bool turn_enabled = !EnvironmentOrDefault("REMOTE_CONTROL_TURN_USERNAME", kDefaultTurnUsername).empty() &&
		!EnvironmentOrDefault("REMOTE_CONTROL_TURN_PASSWORD", kDefaultTurnPassword).empty();
	return EnvironmentOrDefault("REMOTE_CONTROL_STUN_URI", kDefaultStunUri) +
		"  |  " + EnvironmentOrDefault("REMOTE_CONTROL_TURN_UDP_URI", kDefaultTurnUdpUri) +
		"  |  " + EnvironmentOrDefault("REMOTE_CONTROL_TURN_TCP_URI", kDefaultTurnTcpUri) +
		(turn_enabled ? "  [TURN enabled]" : "  [TURN credentials required]");
}

void PeerSession::RequestStats(StatsCallback callback)
{
	auto connection = peer_connection_;
	auto state = stats_request_state_;
	if (!connection || !callback || !state->alive || state->pending.exchange(true)) return;

	struct RequestContext
	{
		std::shared_ptr<StatsRequestState> state;
		std::string remote_user_id;
		StatsCallback callback;
	};
	auto context = std::make_shared<RequestContext>(RequestContext{ state, remote_user_id_, std::move(callback) });
	connection->GetStats(
		[context](const libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>> reports)
		{
			context->state->pending = false;
			if (!context->state->alive || !context->callback) return;
			context->callback(context->remote_user_id, CollectPeerConnectionStats(reports));
		},
		[context](const char*)
		{
			context->state->pending = false;
		});
}

void PeerSession::SetRemoteControlAuthorized(bool authorized)
{
	remote_control_authorized_.store(authorized);
	if (control_channel_) control_channel_->SetAuthorized(authorized);
}

void PeerSession::SetRemoteVideoTarget(HWND target)
{
	remote_video_renderer_ = std::make_unique<RemoteVideoRenderer>(target,
		[this](bool success, int width, int height)
		{
			NotifyMediaEvent(success ? PeerMediaEvent::RemoteVideoFirstFrame :
				PeerMediaEvent::RemoteVideoFrameConversionFailed, width, height);
		});
}

void PeerSession::UpdateRemoteVideoTarget(HWND target)
{
	if (remote_video_renderer_) remote_video_renderer_->SetTarget(target);
}

void PeerSession::SetRecorder(RemoteVideoRecorder* recorder)
{
	if (remote_video_renderer_) remote_video_renderer_->SetRecorder(recorder);
}

void PeerSession::SetStateChangedCallback(StateChangedCallback callback)
{
	state_changed_callback_ = std::move(callback);
}

void PeerSession::SetIceCandidateCallback(IceCandidateCallback callback)
{
	std::lock_guard<std::mutex> lock(signaling_mutex_);
	ice_candidate_callback_ = std::move(callback);
}

void PeerSession::SetMediaEventCallback(MediaEventCallback callback)
{
	std::lock_guard<std::mutex> lock(signaling_mutex_);
	media_event_callback_ = std::move(callback);
}

void PeerSession::SetControlEventCallback(ControlEventCallback callback)
{
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		control_event_callback_ = std::move(callback);
	}
	if (control_channel_)
		control_channel_->SetEventCallback([this](const std::string& detail) { NotifyControlEvent(detail); });
}

void PeerSession::CreateOffer(SdpCallback callback)
{
	if (!peer_connection_) return;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		pending_sdp_callback_ = std::move(callback);
		local_description_announced_ = false;
		remote_description_set_ = false;
		pending_local_candidates_.clear();
		pending_remote_candidates_.clear();
	}
	UpdateState(PeerSessionState::Connecting);
	peer_connection_->CreateOffer(
		[this](const libwebrtc::string& sdp, const libwebrtc::string& type)
		{
			pending_local_sdp_ = sdp;
			pending_local_sdp_type_ = type;
			peer_connection_->SetLocalDescription(pending_local_sdp_, pending_local_sdp_type_, [this]()
				{ AnnounceLocalDescription(); }, [this](const char*) { FailNegotiation(); });
		}, [this](const char*) { FailNegotiation(); }, libwebrtc::RTCMediaConstraints::Create());
}

void PeerSession::ApplyRemoteOffer(const std::string& sdp, SdpCallback answer_callback)
{
	if (!peer_connection_) return;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		pending_sdp_callback_ = std::move(answer_callback);
		local_description_announced_ = false;
		remote_description_set_ = false;
		pending_local_candidates_.clear();
	}
	UpdateState(PeerSessionState::Connecting);
	peer_connection_->SetRemoteDescription(sdp, "offer",
		[this]() {
			MarkRemoteDescriptionSet();
			peer_connection_->CreateAnswer([this](const libwebrtc::string& answer, const libwebrtc::string& type)
				{ pending_local_sdp_ = answer; pending_local_sdp_type_ = type; peer_connection_->SetLocalDescription(pending_local_sdp_, pending_local_sdp_type_, [this]() { AnnounceLocalDescription(); }, [this](const char*) { FailNegotiation(); }); },
				[this](const char*) { FailNegotiation(); }, libwebrtc::RTCMediaConstraints::Create());
		}, [this](const char*) { FailNegotiation(); });
}

void PeerSession::ApplyRemoteAnswer(const std::string& sdp)
{
	if (peer_connection_) peer_connection_->SetRemoteDescription(sdp, "answer",
		[this]() { MarkRemoteDescriptionSet(); }, [this](const char*) { FailNegotiation(); });
}

void PeerSession::AddRemoteCandidate(const std::string& mid, int mline_index, const std::string& candidate)
{
	if (!peer_connection_ || candidate.empty()) return;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		if (!remote_description_set_)
		{
			pending_remote_candidates_.push_back({mid, mline_index, candidate});
			return;
		}
	}
	peer_connection_->AddCandidate(mid, mline_index, candidate);
}

void PeerSession::AnnounceLocalDescription()
{
	SdpCallback sdp_callback;
	IceCandidateCallback ice_callback;
	std::vector<PendingIceCandidate> candidates;
	std::string sdp;
	std::string type;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		local_description_announced_ = true;
		sdp_callback = pending_sdp_callback_;
		ice_callback = ice_candidate_callback_;
		candidates.swap(pending_local_candidates_);
		sdp = pending_local_sdp_.std_string();
		type = pending_local_sdp_type_.std_string();
	}
	// TCP signaling preserves this order: SDP first, then all gathered ICE.
	if (sdp_callback) sdp_callback(sdp, type);
	if (ice_callback)
		for (const auto& item : candidates) ice_callback(item.mid, item.mline_index, item.candidate);
}

void PeerSession::MarkRemoteDescriptionSet()
{
	std::vector<PendingIceCandidate> candidates;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		remote_description_set_ = true;
		candidates.swap(pending_remote_candidates_);
	}
	if (peer_connection_)
		for (const auto& item : candidates) peer_connection_->AddCandidate(item.mid, item.mline_index, item.candidate);
}

void PeerSession::FailNegotiation()
{
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
	}
	UpdateState(PeerSessionState::Failed);
}

void PeerSession::NotifyMediaEvent(PeerMediaEvent event, int width, int height)
{
	MediaEventCallback callback;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		callback = media_event_callback_;
	}
	if (callback) callback(remote_user_id_, event, width, height);
}

void PeerSession::NotifyControlEvent(const std::string& detail)
{
	ControlEventCallback callback;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		callback = control_event_callback_;
	}
	if (callback) callback(remote_user_id_, detail);
}

void PeerSession::PreferCodec(libwebrtc::RTCMediaType media_type, const char* mime_type)
{
	if (!peer_connection_ || !mime_type) return;
	auto factory = engine_.Factory();
	auto capabilities = factory ? factory->GetRtpSenderCapabilities(media_type) : nullptr;
	if (!capabilities) return;
	const auto codecs = capabilities->codecs();
	std::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> ordered;
	bool preferred_codec_found = false;
	for (size_t index = 0; index < codecs.size(); ++index)
	{
		const auto& codec = codecs[index];
		if (codec && _stricmp(codec->mime_type().std_string().c_str(), mime_type) == 0)
		{
			ordered.push_back(codec);
			preferred_codec_found = true;
		}
	}
	if (!preferred_codec_found) return;
	if (_stricmp(mime_type, "video/H264") == 0)
	{
		const auto h264_score = [](const libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>& codec)
			{
				if (!codec) return 1000;
				std::string fmtp = codec->sdp_fmtp_line().std_string();
				std::transform(fmtp.begin(), fmtp.end(), fmtp.begin(),
					[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				int score = fmtp.find("packetization-mode=1") != std::string::npos ? 0 : 100;
				const auto profile = fmtp.find("profile-level-id=");
				if (profile == std::string::npos) return score + 40;
				const std::string prefix = fmtp.substr(profile + 17, 2);
				if (prefix == "64") return score;       // High profile.
				if (prefix == "4d") return score + 10;  // Main profile.
				if (prefix == "42") return score + 20;  // Baseline/constrained baseline.
				return score + 30;
			};
		std::stable_sort(ordered.begin(), ordered.end(),
			[&h264_score](const auto& left, const auto& right)
			{
				return h264_score(left) < h264_score(right);
			});
	}
	for (size_t index = 0; index < codecs.size(); ++index)
	{
		const auto& codec = codecs[index];
		if (!codec || _stricmp(codec->mime_type().std_string().c_str(), mime_type) != 0)
			ordered.push_back(codec);
	}

	const auto transceivers = peer_connection_->transceivers();
	const libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>>
		codec_preferences(ordered);
	for (size_t index = 0; index < transceivers.size(); ++index)
	{
		const auto& transceiver = transceivers[index];
		if (transceiver && transceiver->media_type() == media_type)
			transceiver->SetCodecPreferences(codec_preferences);
	}
}

void PeerSession::UpdateState(PeerSessionState state)
{
	if (state_.load() == state)
	{
		return;
	}
	state_.store(state);
	if (state_changed_callback_)
	{
		state_changed_callback_(remote_user_id_, state);
	}
}

void PeerSession::OnSignalingState(libwebrtc::RTCSignalingState) {}
void PeerSession::OnIceGatheringState(libwebrtc::RTCIceGatheringState) {}
void PeerSession::OnIceConnectionState(libwebrtc::RTCIceConnectionState) {}
void PeerSession::OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate)
{
	if (!candidate) return;
	libwebrtc::string text;
	if (!candidate->ToString(text)) return;
	PendingIceCandidate item{candidate->sdp_mid().std_string(), candidate->sdp_mline_index(), text.std_string()};
	IceCandidateCallback callback;
	{
		std::lock_guard<std::mutex> lock(signaling_mutex_);
		if (closed_) return;
		if (!local_description_announced_)
		{
			pending_local_candidates_.push_back(std::move(item));
			return;
		}
		callback = ice_candidate_callback_;
	}
	if (callback) callback(item.mid, item.mline_index, item.candidate);
}
void PeerSession::OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) {}
void PeerSession::OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) {}
void PeerSession::OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> channel)
{
	if (!options_.enable_remote_control || !channel ||
		channel->label().std_string() != "remote-control") return;
	if (!control_channel_) control_channel_ = std::make_unique<RemoteControlChannel>();
	control_channel_->SetEventCallback([this](const std::string& detail) { NotifyControlEvent(detail); });
	control_channel_->SetAuthorized(remote_control_authorized_.load());
	control_channel_->Attach(channel);
}
void PeerSession::OnRenegotiationNeeded() {}
void PeerSession::OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver)
{
	if (!transceiver || !remote_video_renderer_) return;
	auto track = transceiver->receiver()->track();
	if (track && track->kind().std_string() == "video")
	{
		// libwebrtc objects cross a DLL boundary. MSVC dynamic_cast consults RTTI
		// owned by both modules and crashes with this prebuilt wrapper. The WebRTC
		// contract guarantees that a track whose kind is "video" is RTCVideoTrack.
		auto* video = static_cast<libwebrtc::RTCVideoTrack*>(track.get());
		remote_video_track_ = video;
		remote_video_track_->set_enabled(true);
		remote_video_track_->AddRenderer(remote_video_renderer_.get());
		NotifyMediaEvent(PeerMediaEvent::RemoteVideoTrackAdded);
	}
}
void PeerSession::OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>, libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) {}
void PeerSession::OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) {}

void PeerSession::OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state)
{
	switch (state)
	{
	case libwebrtc::RTCPeerConnectionStateConnecting: UpdateState(PeerSessionState::Connecting); break;
	case libwebrtc::RTCPeerConnectionStateConnected: UpdateState(PeerSessionState::Connected); break;
	case libwebrtc::RTCPeerConnectionStateDisconnected: UpdateState(PeerSessionState::Disconnected); break;
	case libwebrtc::RTCPeerConnectionStateFailed: UpdateState(PeerSessionState::Failed); break;
	case libwebrtc::RTCPeerConnectionStateClosed: UpdateState(PeerSessionState::Closed); break;
	default: break;
	}
}
