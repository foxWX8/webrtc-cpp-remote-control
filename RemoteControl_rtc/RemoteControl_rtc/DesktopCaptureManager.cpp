#include "pch.h"
#include "DesktopCaptureManager.h"

#include "DxgiDesktopCapturer.h"

bool DesktopCaptureManager::StartPrimaryScreen(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory)
{
	Stop();
	if (!factory) return false;
	video_source_ = factory->CreateCustomVideoSource("dxgi-desktop-source", libwebrtc::RTCMediaConstraints::Create());
	if (!video_source_) { Stop(); return false; }
	video_track_ = factory->CreateVideoTrack(video_source_, "desktop-video");
	capturer_ = std::make_unique<DxgiDesktopCapturer>();
	if (!video_track_ || !capturer_->Start(video_source_, 30)) { Stop(); return false; }
	video_track_->set_enabled(true);
	return true;
}

void DesktopCaptureManager::Stop()
{
	if (capturer_) capturer_->Stop();
	video_track_ = nullptr;
	video_source_ = nullptr;
	capturer_.reset();
}
