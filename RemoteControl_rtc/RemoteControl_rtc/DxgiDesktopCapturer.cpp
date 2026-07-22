#include "pch.h"
#include "DxgiDesktopCapturer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <thread>
#include <vector>
#include <wrl/client.h>

#include "rtc_video_frame.h"

using Microsoft::WRL::ComPtr;

namespace
{
	uint8_t ClampByte(int value)
	{
		return static_cast<uint8_t>((std::max)(0, (std::min)(255, value)));
	}

	void ConvertBgraToI420(const uint8_t* bgra, int bgra_stride, int width, int height,
		std::vector<uint8_t>& y_plane, std::vector<uint8_t>& u_plane, std::vector<uint8_t>& v_plane)
	{
		const int chroma_width = (width + 1) / 2;
		const int chroma_height = (height + 1) / 2;
		y_plane.resize(static_cast<size_t>(width) * height);
		u_plane.resize(static_cast<size_t>(chroma_width) * chroma_height);
		v_plane.resize(static_cast<size_t>(chroma_width) * chroma_height);

		for (int y = 0; y < height; ++y)
		{
			const uint8_t* const source_row = bgra + static_cast<size_t>(y) * bgra_stride;
			uint8_t* const destination_row = y_plane.data() + static_cast<size_t>(y) * width;
			for (int x = 0; x < width; ++x)
			{
				const uint8_t* const pixel = source_row + static_cast<size_t>(x) * 4;
				const int blue = pixel[0];
				const int green = pixel[1];
				const int red = pixel[2];
				destination_row[x] = ClampByte(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
			}
		}

		for (int y = 0; y < height; y += 2)
		{
			for (int x = 0; x < width; x += 2)
			{
				int red_sum = 0;
				int green_sum = 0;
				int blue_sum = 0;
				int sample_count = 0;
				for (int offset_y = 0; offset_y < 2 && y + offset_y < height; ++offset_y)
				{
					const uint8_t* const row = bgra + static_cast<size_t>(y + offset_y) * bgra_stride;
					for (int offset_x = 0; offset_x < 2 && x + offset_x < width; ++offset_x)
					{
						const uint8_t* const pixel = row + static_cast<size_t>(x + offset_x) * 4;
						blue_sum += pixel[0];
						green_sum += pixel[1];
						red_sum += pixel[2];
						++sample_count;
					}
				}
				const int red = red_sum / sample_count;
				const int green = green_sum / sample_count;
				const int blue = blue_sum / sample_count;
				const size_t chroma_index = static_cast<size_t>(y / 2) * chroma_width + x / 2;
				u_plane[chroma_index] = ClampByte(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
				v_plane[chroma_index] = ClampByte(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
			}
		}
	}
}

struct DxgiDesktopCapturer::Impl
{
	bool Start(libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> new_source, unsigned int requested_fps)
	{
		Stop();
		if (!new_source || requested_fps == 0) return false;
		source = std::move(new_source);
		fps = requested_fps;
		stop_requested = false;
		startup_complete = false;
		startup_success = false;
		worker = std::thread(&Impl::Run, this);

		std::unique_lock<std::mutex> lock(startup_mutex);
		if (!startup_condition.wait_for(lock, std::chrono::seconds(5), [this] { return startup_complete; }) ||
			!startup_success)
		{
			lock.unlock();
			Stop();
			return false;
		}
		return true;
	}

	void Stop()
	{
		stop_requested = true;
		if (worker.joinable()) worker.join();
		running = false;
		source = nullptr;
		ReleaseDxgi();
	}

	bool InitializeDxgi()
	{
		ReleaseDxgi();
		ComPtr<IDXGIFactory1> factory;
		if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

		const HMONITOR primary_monitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
		ComPtr<IDXGIAdapter1> selected_adapter;
		ComPtr<IDXGIOutput> selected_output;
		bool found_primary = false;
		for (UINT adapter_index = 0;; ++adapter_index)
		{
			ComPtr<IDXGIAdapter1> candidate_adapter;
			if (factory->EnumAdapters1(adapter_index, &candidate_adapter) == DXGI_ERROR_NOT_FOUND) break;
			for (UINT output_index = 0;; ++output_index)
			{
				ComPtr<IDXGIOutput> candidate_output;
				if (candidate_adapter->EnumOutputs(output_index, &candidate_output) == DXGI_ERROR_NOT_FOUND) break;
				DXGI_OUTPUT_DESC output_description{};
				if (FAILED(candidate_output->GetDesc(&output_description)) || !output_description.AttachedToDesktop) continue;
				if (!selected_output || output_description.Monitor == primary_monitor)
				{
					selected_adapter = candidate_adapter;
					selected_output = candidate_output;
					found_primary = output_description.Monitor == primary_monitor;
				}
				if (found_primary) break;
			}
			if (found_primary) break;
		}
		if (!selected_adapter || !selected_output) return false;

		UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		D3D_FEATURE_LEVEL feature_level{};
		if (FAILED(::D3D11CreateDevice(selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			device_flags, nullptr, 0, D3D11_SDK_VERSION, &device, &feature_level, &context))) return false;

		ComPtr<IDXGIOutput1> output1;
		if (FAILED(selected_output.As(&output1))) return false;
		if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) return false;
		duplication->GetDesc(&duplication_description);
		return duplication_description.ModeDesc.Width > 0 && duplication_description.ModeDesc.Height > 0;
	}

	void ReleaseDxgi()
	{
		staging_texture.Reset();
		duplication.Reset();
		context.Reset();
		device.Reset();
		staging_width = 0;
		staging_height = 0;
	}

	bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& source_description)
	{
		if (staging_texture && staging_width == source_description.Width &&
			staging_height == source_description.Height && staging_format == source_description.Format) return true;
		staging_texture.Reset();
		D3D11_TEXTURE2D_DESC description = source_description;
		description.BindFlags = 0;
		description.MiscFlags = 0;
		description.Usage = D3D11_USAGE_STAGING;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateTexture2D(&description, nullptr, &staging_texture))) return false;
		staging_width = description.Width;
		staging_height = description.Height;
		staging_format = description.Format;
		return true;
	}

	bool RecoverDuplication()
	{
		ReleaseDxgi();
		while (!stop_requested)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			if (InitializeDxgi()) return true;
		}
		return false;
	}

	void Run()
	{
		const bool initialized = InitializeDxgi();
		{
			std::lock_guard<std::mutex> lock(startup_mutex);
			startup_success = initialized;
			startup_complete = true;
		}
		startup_condition.notify_one();
		if (!initialized) return;

		running = true;
		const auto frame_interval = std::chrono::milliseconds((std::max)(1U, 1000U / fps));
		auto next_frame_time = std::chrono::steady_clock::now();
		while (!stop_requested)
		{
			DXGI_OUTDUPL_FRAME_INFO frame_information{};
			ComPtr<IDXGIResource> desktop_resource;
			const HRESULT acquire_result = duplication->AcquireNextFrame(100, &frame_information, &desktop_resource);
			if (acquire_result == DXGI_ERROR_WAIT_TIMEOUT) continue;
			if (acquire_result == DXGI_ERROR_ACCESS_LOST)
			{
				if (!RecoverDuplication()) break;
				continue;
			}
			if (FAILED(acquire_result)) break;

			const auto now = std::chrono::steady_clock::now();
			if (now < next_frame_time)
			{
				duplication->ReleaseFrame();
				continue;
			}

			ComPtr<ID3D11Texture2D> desktop_texture;
			if (FAILED(desktop_resource.As(&desktop_texture)))
			{
				duplication->ReleaseFrame();
				continue;
			}
			D3D11_TEXTURE2D_DESC texture_description{};
			desktop_texture->GetDesc(&texture_description);
			if (texture_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || !EnsureStagingTexture(texture_description))
			{
				duplication->ReleaseFrame();
				continue;
			}

			context->CopyResource(staging_texture.Get(), desktop_texture.Get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
			{
				ConvertBgraToI420(static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
					static_cast<int>(texture_description.Width), static_cast<int>(texture_description.Height),
					y_plane, u_plane, v_plane);
				context->Unmap(staging_texture.Get(), 0);

				const int width = static_cast<int>(texture_description.Width);
				const int height = static_cast<int>(texture_description.Height);
				const int chroma_stride = (width + 1) / 2;
				auto video_frame = libwebrtc::RTCVideoFrame::Create(width, height,
					y_plane.data(), width, u_plane.data(), chroma_stride, v_plane.data(), chroma_stride);
				if (video_frame && source) source->OnCapturedFrame(video_frame);
				next_frame_time = now + frame_interval;
			}
			duplication->ReleaseFrame();
		}
		running = false;
		ReleaseDxgi();
	}

	std::thread worker;
	std::atomic<bool> stop_requested = false;
	std::atomic<bool> running = false;
	std::mutex startup_mutex;
	std::condition_variable startup_condition;
	bool startup_complete = false;
	bool startup_success = false;
	unsigned int fps = 30;
	libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> source;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutputDuplication> duplication;
	ComPtr<ID3D11Texture2D> staging_texture;
	DXGI_OUTDUPL_DESC duplication_description{};
	UINT staging_width = 0;
	UINT staging_height = 0;
	DXGI_FORMAT staging_format = DXGI_FORMAT_UNKNOWN;
	std::vector<uint8_t> y_plane;
	std::vector<uint8_t> u_plane;
	std::vector<uint8_t> v_plane;
};

DxgiDesktopCapturer::DxgiDesktopCapturer() : impl_(std::make_unique<Impl>()) {}
DxgiDesktopCapturer::~DxgiDesktopCapturer() { Stop(); }

bool DxgiDesktopCapturer::Start(libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> source, unsigned int fps)
{
	return impl_->Start(std::move(source), fps);
}

void DxgiDesktopCapturer::Stop() { impl_->Stop(); }
bool DxgiDesktopCapturer::IsRunning() const { return impl_->running; }
