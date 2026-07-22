#include "pch.h"
#include "RemoteVideoRenderer.h"
#include "VideoRenderWnd.h"
#include "RemoteVideoRecorder.h"

namespace
{
	uint8_t ClampToByte(int value)
	{
		return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
	}

	bool ConvertI420ToBgra(const libwebrtc::RTCVideoFrame& frame, VideoFrameData& output)
	{
		const uint8_t* const data_y = frame.DataY();
		const uint8_t* const data_u = frame.DataU();
		const uint8_t* const data_v = frame.DataV();
		const int stride_y = frame.StrideY();
		const int stride_u = frame.StrideU();
		const int stride_v = frame.StrideV();
		if (!data_y || !data_u || !data_v || stride_y <= 0 || stride_u <= 0 || stride_v <= 0)
			return false;

		for (int y = 0; y < output.height; ++y)
		{
			const uint8_t* const row_y = data_y + static_cast<size_t>(y) * stride_y;
			const uint8_t* const row_u = data_u + static_cast<size_t>(y / 2) * stride_u;
			const uint8_t* const row_v = data_v + static_cast<size_t>(y / 2) * stride_v;
			uint8_t* const row_bgra = output.pixels.data() +
				static_cast<size_t>(y) * output.width * 4;

			for (int x = 0; x < output.width; ++x)
			{
				const int c = (std::max)(0, static_cast<int>(row_y[x]) - 16);
				const int d = static_cast<int>(row_u[x / 2]) - 128;
				const int e = static_cast<int>(row_v[x / 2]) - 128;
				uint8_t* const pixel = row_bgra + static_cast<size_t>(x) * 4;
				pixel[0] = ClampToByte((298 * c + 516 * d + 128) >> 8);
				pixel[1] = ClampToByte((298 * c - 100 * d - 208 * e + 128) >> 8);
				pixel[2] = ClampToByte((298 * c + 409 * e + 128) >> 8);
				pixel[3] = 255;
			}
		}
		return true;
	}
}

void RemoteVideoRenderer::OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame)
{
	const HWND target = target_.load();
	if (!frame || !::IsWindow(target)) return;
	auto output = std::make_unique<VideoFrameData>();
	output->width = frame->width(); output->height = frame->height();
	if (output->width <= 0 || output->height <= 0) return;
	output->pixels.resize(static_cast<size_t>(output->width) * output->height * 4);
	const int conversion_result = frame->ConvertToARGB(libwebrtc::RTCVideoFrame::Type::kBGRA,
		output->pixels.data(), output->width * 4, output->width, output->height);
	const bool converted = conversion_result == 0 || ConvertI420ToBgra(*frame, *output);
	if (converted)
	{
		if (!first_frame_reported_.exchange(true) && frame_status_callback_)
			frame_status_callback_(true, output->width, output->height);
		if (recorder_) recorder_->PushFrame(*output);
		auto* raw = output.release();
		if (!::PostMessage(target, WM_APP + 201, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
	}
	else if (!conversion_failure_reported_.exchange(true) && frame_status_callback_)
	{
		frame_status_callback_(false, output->width, output->height);
	}
}
