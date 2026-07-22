#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "VideoRenderWnd.h"

class RemoteVideoRecorder final
{
public:
	~RemoteVideoRecorder();
	bool Start(const std::wstring& path);
	void Stop();
	bool IsRecording() const { return recording_; }
	void PushFrame(const VideoFrameData& frame);

private:
	void Run();
	std::wstring path_;
	std::atomic<bool> recording_ = false;
	bool stop_requested_ = false;
	std::mutex mutex_;
	std::condition_variable condition_;
	std::deque<std::unique_ptr<VideoFrameData>> frames_;
	std::thread worker_;
};
