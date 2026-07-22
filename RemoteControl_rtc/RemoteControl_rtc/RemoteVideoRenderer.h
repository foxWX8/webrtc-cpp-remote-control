#pragma once

#include <atomic>
#include <functional>

#include "rtc_video_renderer.h"
#include "rtc_video_frame.h"

class RemoteVideoRenderer final : public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>>
{
public:
	using FrameStatusCallback = std::function<void(bool, int, int)>;
	explicit RemoteVideoRenderer(HWND target, FrameStatusCallback callback = {})
		: target_(target), frame_status_callback_(std::move(callback)) {}
	void SetTarget(HWND target) { target_.store(target); }
	void SetRecorder(class RemoteVideoRecorder* recorder) { recorder_ = recorder; }
	void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override;
private:
	std::atomic<HWND> target_ = nullptr;
	class RemoteVideoRecorder* recorder_ = nullptr;
	FrameStatusCallback frame_status_callback_;
	std::atomic<bool> first_frame_reported_ = false;
	std::atomic<bool> conversion_failure_reported_ = false;
};
