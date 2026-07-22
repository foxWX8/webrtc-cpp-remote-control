#include "pch.h"
#include "LocalAudioManager.h"

bool LocalAudioManager::Start(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory)
{
	if (audio_track_) return true;
	if (!factory) return false;
	libwebrtc::RTCAudioOptions options;
	audio_source_ = factory->CreateAudioSource("microphone-source", libwebrtc::RTCAudioSource::SourceType::kMicrophone, options);
	if (!audio_source_) return false;
	audio_track_ = factory->CreateAudioTrack(audio_source_, "microphone-audio");
	if (!audio_track_) { Stop(); return false; }
	audio_track_->set_enabled(true);
	return true;
}

void LocalAudioManager::Stop()
{
	if (audio_track_) audio_track_->set_enabled(false);
	audio_track_ = nullptr;
	audio_source_ = nullptr;
}
