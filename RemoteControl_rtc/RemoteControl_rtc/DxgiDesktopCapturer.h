#pragma once

#include <memory>

#include "rtc_types.h"
#include "rtc_video_source.h"

// Captures the primary monitor with DXGI Desktop Duplication and pushes I420
// frames into a libwebrtc custom video source.
class DxgiDesktopCapturer final
{
public:
	DxgiDesktopCapturer();
	~DxgiDesktopCapturer();

	bool Start(libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> source, unsigned int fps);
	void Stop();
	bool IsRunning() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
