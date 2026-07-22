#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MediaOptions.h"
#include "rtc_peerconnection.h"

class WebRtcEngine;
class RemoteVideoRenderer;
class RemoteControlChannel;

enum class PeerSessionState
{
	New,
	Connecting,
	Connected,
	Disconnected,
	Failed,
	Closed,
};

enum class PeerMediaEvent
{
	RemoteVideoTrackAdded,
	RemoteVideoFirstFrame,
	RemoteVideoFrameConversionFailed,
};

struct PeerConnectionStats
{
	bool video_stats_available = false;
	bool audio_stats_available = false;
	bool video_jitter_available = false;
	bool audio_jitter_available = false;
	bool video_buffer_available = false;
	bool round_trip_time_available = false;
	bool available_incoming_bitrate_available = false;
	int64_t sample_time_us = 0;
	uint64_t video_bytes_received = 0;
	uint64_t audio_bytes_received = 0;
	uint64_t video_packets_received = 0;
	uint64_t audio_packets_received = 0;
	int64_t video_packets_lost = 0;
	int64_t audio_packets_lost = 0;
	uint64_t video_frames_decoded = 0;
	double video_frames_per_second = 0.0;
	int video_width = 0;
	int video_height = 0;
	double video_jitter_ms = 0.0;
	double audio_jitter_ms = 0.0;
	double video_jitter_buffer_delay_seconds = 0.0;
	uint64_t video_jitter_buffer_emitted_count = 0;
	double video_playout_delay_ms = 0.0;
	double round_trip_time_ms = 0.0;
	double available_incoming_bitrate_bps = 0.0;
	std::string ice_route;
};

class PeerSession final : private libwebrtc::RTCPeerConnectionObserver
{
public:
	using StateChangedCallback = std::function<void(const std::string&, PeerSessionState)>;
	using SdpCallback = std::function<void(const std::string&, const std::string&)>;
	using IceCandidateCallback = std::function<void(const std::string&, int, const std::string&)>;
	using MediaEventCallback = std::function<void(const std::string&, PeerMediaEvent, int, int)>;
	using ControlEventCallback = std::function<void(const std::string&, const std::string&)>;
	using StatsCallback = std::function<void(const std::string&, const PeerConnectionStats&)>;

	PeerSession(WebRtcEngine& engine, std::string remote_user_id, MediaOptions options);
	~PeerSession();

	PeerSession(const PeerSession&) = delete;
	PeerSession& operator=(const PeerSession&) = delete;

	bool Create();
	bool AddLocalVideoTrack(libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track);
	bool AddLocalAudioTrack(libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> track);
	bool CreateControlChannel();
	void SetRemoteVideoTarget(HWND target);
	void UpdateRemoteVideoTarget(HWND target);
	void SetRecorder(class RemoteVideoRecorder* recorder);
	void SetRemoteControlAuthorized(bool authorized);
	RemoteControlChannel* ControlChannel() const { return control_channel_.get(); }
	void CreateOffer(SdpCallback callback);
	void ApplyRemoteOffer(const std::string& sdp, SdpCallback answer_callback);
	void ApplyRemoteAnswer(const std::string& sdp);
	void AddRemoteCandidate(const std::string& mid, int mline_index, const std::string& candidate);
	void Close();
	const std::string& RemoteUserId() const { return remote_user_id_; }
	PeerSessionState State() const { return state_.load(); }
	const MediaOptions& Options() const { return options_; }
	static std::string IceServiceSummary();
	void RequestStats(StatsCallback callback);
	void SetStateChangedCallback(StateChangedCallback callback);
	void SetIceCandidateCallback(IceCandidateCallback callback);
	void SetMediaEventCallback(MediaEventCallback callback);
	void SetControlEventCallback(ControlEventCallback callback);

private:
	struct PendingIceCandidate
	{
		std::string mid;
		int mline_index = 0;
		std::string candidate;
	};

	void AnnounceLocalDescription();
	void MarkRemoteDescriptionSet();
	void FailNegotiation();
	void NotifyMediaEvent(PeerMediaEvent event, int width = 0, int height = 0);
	void NotifyControlEvent(const std::string& detail);
	void PreferCodec(libwebrtc::RTCMediaType media_type, const char* mime_type);
	void UpdateState(PeerSessionState state);
	void OnSignalingState(libwebrtc::RTCSignalingState state) override;
	void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override;
	void OnIceGatheringState(libwebrtc::RTCIceGatheringState state) override;
	void OnIceConnectionState(libwebrtc::RTCIceConnectionState state) override;
	void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override;
	void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream> stream) override;
	void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream> stream) override;
	void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> data_channel) override;
	void OnRenegotiationNeeded() override;
	void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver) override;
	void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>> streams,
		libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override;
	void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override;

	WebRtcEngine& engine_;
	std::string remote_user_id_;
	MediaOptions options_;
	std::atomic<PeerSessionState> state_{ PeerSessionState::New };
	StateChangedCallback state_changed_callback_;
	IceCandidateCallback ice_candidate_callback_;
	MediaEventCallback media_event_callback_;
	ControlEventCallback control_event_callback_;
	SdpCallback pending_sdp_callback_;
	std::mutex signaling_mutex_;
	bool closed_ = false;
	bool local_description_announced_ = false;
	bool remote_description_set_ = false;
	std::vector<PendingIceCandidate> pending_local_candidates_;
	std::vector<PendingIceCandidate> pending_remote_candidates_;
	libwebrtc::string pending_local_sdp_;
	libwebrtc::string pending_local_sdp_type_;
	libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer_connection_;
	libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> remote_video_track_;
	std::unique_ptr<RemoteVideoRenderer> remote_video_renderer_;
	std::unique_ptr<RemoteControlChannel> control_channel_;
	std::atomic<bool> remote_control_authorized_ = false;
	struct StatsRequestState;
	std::shared_ptr<StatsRequestState> stats_request_state_;
};
