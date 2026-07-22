#include "pch.h"
#include "RemoteVideoRecorder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr UINT32 kRecordingFramesPerSecond = 30;
	constexpr LONGLONG kHundredNanosecondsPerSecond = 10000000LL;

	bool CreateMp4Writer(const std::wstring& path, UINT32 width, UINT32 height,
		ComPtr<IMFSinkWriter>& writer, DWORD& stream_index)
	{
		ComPtr<IMFAttributes> attributes;
		if (FAILED(::MFCreateAttributes(&attributes, 2))) return false;
		attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
		if (FAILED(::MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer))) return false;

		ComPtr<IMFMediaType> output_type;
		if (FAILED(::MFCreateMediaType(&output_type))) return false;
		output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		// Use a high desktop-friendly bitrate. Congestion-control bitrate used by
		// WebRTC does not apply to this local recording encoder.
		const UINT32 bitrate = (std::max)(6000000u, (std::min)(20000000u, width * height * 6u));
		output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
		output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (FAILED(::MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
			FAILED(::MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, kRecordingFramesPerSecond, 1)) ||
			FAILED(::MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
			FAILED(writer->AddStream(output_type.Get(), &stream_index))) return false;

		ComPtr<IMFMediaType> input_type;
		if (FAILED(::MFCreateMediaType(&input_type))) return false;
		input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		input_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
		input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
		input_type->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 4u);
		input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4u);
		if (FAILED(::MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
			FAILED(::MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, kRecordingFramesPerSecond, 1)) ||
			FAILED(::MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
			FAILED(writer->SetInputMediaType(stream_index, input_type.Get(), nullptr)) ||
			FAILED(writer->BeginWriting())) return false;
		return true;
	}
}

RemoteVideoRecorder::~RemoteVideoRecorder() { Stop(); }

bool RemoteVideoRecorder::Start(const std::wstring& path)
{
	if (path.empty() || recording_.load()) return false;
	Stop();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		path_ = path;
		stop_requested_ = false;
		frames_.clear();
		recording_ = true;
	}
	worker_ = std::thread(&RemoteVideoRecorder::Run, this);
	return true;
}

void RemoteVideoRecorder::Stop()
{
	recording_ = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_requested_ = true;
		frames_.clear();
	}
	condition_.notify_all();
	if (worker_.joinable()) worker_.join();
}

void RemoteVideoRecorder::PushFrame(const VideoFrameData& frame)
{
	if (!recording_.load() || frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) return;
	auto copy = std::make_unique<VideoFrameData>(frame);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!recording_.load() || stop_requested_) return;
		if (frames_.size() >= 6) frames_.pop_front();
		frames_.push_back(std::move(copy));
	}
	condition_.notify_one();
}

void RemoteVideoRecorder::Run()
{
	const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool uninitialize_com = SUCCEEDED(com_result);
	if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_FULL)))
	{
		recording_ = false;
		if (uninitialize_com) ::CoUninitialize();
		return;
	}

	ComPtr<IMFSinkWriter> writer;
	DWORD stream_index = 0;
	LONGLONG frame_index = 0;
	int width = 0;
	int height = 0;
	for (;;)
	{
		std::unique_ptr<VideoFrameData> frame;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			condition_.wait(lock, [this] { return stop_requested_ || !frames_.empty(); });
			if (stop_requested_) break;
			frame = std::move(frames_.front());
			frames_.pop_front();
		}

		if (!writer)
		{
			width = frame->width;
			height = frame->height;
			if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 ||
				!CreateMp4Writer(path_, static_cast<UINT32>(width), static_cast<UINT32>(height),
					writer, stream_index)) break;
		}
		if (frame->width != width || frame->height != height) continue;

		const size_t stride = static_cast<size_t>(width) * 4;
		const size_t frame_bytes = stride * height;
		if (frame->pixels.size() < frame_bytes) continue;
		ComPtr<IMFMediaBuffer> buffer;
		if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(frame_bytes), &buffer))) break;
		BYTE* destination = nullptr;
		if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) break;
		// RemoteVideoRenderer produces top-down BGRA. Media Foundation's sink
		// writer accepts this order directly; the old AVI path required a
		// bottom-up DIB and must not be applied to MP4/H.264 input.
		memcpy(destination, frame->pixels.data(), frame_bytes);
		buffer->Unlock();
		if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(frame_bytes)))) break;

		ComPtr<IMFSample> sample;
		if (FAILED(::MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) break;
		const LONGLONG sample_time = frame_index * kHundredNanosecondsPerSecond /
			kRecordingFramesPerSecond;
		const LONGLONG next_sample_time = (frame_index + 1) * kHundredNanosecondsPerSecond /
			kRecordingFramesPerSecond;
		sample->SetSampleTime(sample_time);
		sample->SetSampleDuration(next_sample_time - sample_time);
		if (FAILED(writer->WriteSample(stream_index, sample.Get()))) break;
		++frame_index;
	}

	if (writer) writer->Finalize();
	writer.Reset();
	::MFShutdown();
	if (uninitialize_com) ::CoUninitialize();
	recording_ = false;
}
