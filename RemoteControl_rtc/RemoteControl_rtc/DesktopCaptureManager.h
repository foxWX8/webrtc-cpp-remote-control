#pragma once

#include "rtc_peerconnection_factory.h"
#include "rtc_video_source.h"
#include "DxgiDesktopCapturer.h"

// Owns the selected screen capturer and its local WebRTC video track.
class DesktopCaptureManager final
{
public:
	bool StartPrimaryScreen(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory);
	void Stop();
	libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> VideoTrack() const { return video_track_; }

private:
	std::unique_ptr<DxgiDesktopCapturer> capturer_;
	libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> video_source_;
	libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> video_track_;
};
