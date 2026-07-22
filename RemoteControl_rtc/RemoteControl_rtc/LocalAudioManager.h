#pragma once

#include "rtc_peerconnection_factory.h"

class LocalAudioManager final
{
public:
	bool Start(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory);
	void Stop();
	libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> AudioTrack() const { return audio_track_; }

private:
	libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> audio_source_;
	libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> audio_track_;
};
