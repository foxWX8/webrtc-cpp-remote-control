#include "pch.h"
#include "VideoRenderWnd.h"
#include "InputInjectorWin.h"

#include <algorithm>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr UINT WM_VIDEO_FRAME = WM_APP + 201;

	constexpr char kVertexShader[] = R"(
struct VertexOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertex_id : SV_VertexID) {
    const float2 positions[4] = {
        float2(-1.0,  1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0, -1.0)
    };
    const float2 coordinates[4] = {
        float2(0.0, 0.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = coordinates[vertex_id];
    return output;
})";

	constexpr char kPixelShader[] = R"(
Texture2D video_texture : register(t0);
SamplerState video_sampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return video_texture.Sample(video_sampler, uv);
})";

	bool CompileShader(const char* source, const char* target, ComPtr<ID3DBlob>& bytecode)
	{
		ComPtr<ID3DBlob> errors;
		return SUCCEEDED(::D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
			"main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors));
	}
}

struct VideoRenderWnd::D3dState
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGISwapChain> swap_chain;
	ComPtr<ID3D11RenderTargetView> render_target;
	ComPtr<ID3D11VertexShader> vertex_shader;
	ComPtr<ID3D11PixelShader> pixel_shader;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11Texture2D> video_texture;
	ComPtr<ID3D11ShaderResourceView> video_view;
	int client_width = 0;
	int client_height = 0;
	int texture_width = 0;
	int texture_height = 0;
};

BEGIN_MESSAGE_MAP(VideoRenderWnd, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_DESTROY()
	ON_WM_SIZE()
	ON_MESSAGE(WM_VIDEO_FRAME, &VideoRenderWnd::OnFrame)
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_RBUTTONUP()
	ON_WM_MBUTTONDOWN()
	ON_WM_MBUTTONUP()
	ON_WM_MOUSEWHEEL()
	ON_WM_MOUSEACTIVATE()
	ON_WM_ACTIVATE()
	ON_WM_KEYDOWN()
	ON_WM_KEYUP()
	ON_WM_CLOSE()
END_MESSAGE_MAP()

VideoRenderWnd::VideoRenderWnd() = default;
VideoRenderWnd::~VideoRenderWnd() { ReleaseD3d(); }

void VideoRenderWnd::SetInputCallback(InputCallback callback)
{
	input_callback_ = std::move(callback);
	if (fullscreen_view_) fullscreen_view_->input_callback_ = input_callback_;
}

BOOL VideoRenderWnd::Create(const RECT& rect, CWnd* parent, UINT id)
{
	CString class_name = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
		::LoadCursor(nullptr, IDC_ARROW), nullptr);
	return CWnd::CreateEx(0, class_name, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP, rect, parent, id);
}

BOOL VideoRenderWnd::CreateFullscreen(const RECT& rect, CWnd* owner, VideoRenderWnd* fullscreen_owner)
{
	fullscreen_owner_ = fullscreen_owner;
	CString class_name = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
		::LoadCursor(nullptr, IDC_ARROW), nullptr);
	const BOOL created = CWnd::CreateEx(WS_EX_TOPMOST, class_name, L"远程视频",
		WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP,
		rect, owner, 0);
	if (!created)
	{
		fullscreen_owner_ = nullptr;
	}
	else
	{
		// Keep the fullscreen window visible in complete-desktop capture.
		::SetWindowDisplayAffinity(m_hWnd, WDA_NONE);
	}
	return created;
}

void VideoRenderWnd::ForwardInput(UINT message, WPARAM wparam, LPARAM lparam)
{
	if (InputInjectorWin::IsInjectedMessage()) return;
	InputCallback* callback = fullscreen_owner_ ? &fullscreen_owner_->input_callback_ : &input_callback_;
	if (*callback) (*callback)(this, message, wparam, lparam);
}

void VideoRenderWnd::OnMouseMove(UINT flags, CPoint point)
{
	ForwardInput(WM_MOUSEMOVE, flags, MAKELPARAM(point.x, point.y));
}

void VideoRenderWnd::OnLButtonDown(UINT flags, CPoint point)
{
	SetFocus();
	SetCapture();
	ForwardInput(WM_LBUTTONDOWN, flags, MAKELPARAM(point.x, point.y));
}
void VideoRenderWnd::OnLButtonDblClk(UINT flags, CPoint point)
{
	if (InputInjectorWin::IsInjectedMessage()) return;
	SetFocus();
	SetCapture();
	// Windows replaces the second WM_LBUTTONDOWN with WM_LBUTTONDBLCLK for a
	// CS_DBLCLKS window. Forward it as the second normal down event; the
	// following WM_LBUTTONUP completes the second click on the remote machine.
	// Fullscreen is deliberately controlled only by the button or F11 so it no
	// longer consumes remote desktop double-clicks in the main video view.
	ForwardInput(WM_LBUTTONDOWN, flags, MAKELPARAM(point.x, point.y));
}
void VideoRenderWnd::OnLButtonUp(UINT flags, CPoint point)
{
	ForwardInput(WM_LBUTTONUP, flags, MAKELPARAM(point.x, point.y));
	if (!fullscreen_owner_ && ::GetCapture() == m_hWnd) ReleaseCapture();
}

void VideoRenderWnd::OnRButtonDown(UINT flags, CPoint point)
{
	SetFocus();
	SetCapture();
	ForwardInput(WM_RBUTTONDOWN, flags, MAKELPARAM(point.x, point.y));
}

void VideoRenderWnd::OnRButtonUp(UINT flags, CPoint point)
{
	ForwardInput(WM_RBUTTONUP, flags, MAKELPARAM(point.x, point.y));
	if (!fullscreen_owner_ && ::GetCapture() == m_hWnd) ReleaseCapture();
}

void VideoRenderWnd::OnMButtonDown(UINT flags, CPoint point)
{
	SetFocus();
	SetCapture();
	ForwardInput(WM_MBUTTONDOWN, flags, MAKELPARAM(point.x, point.y));
}

void VideoRenderWnd::OnMButtonUp(UINT flags, CPoint point)
{
	ForwardInput(WM_MBUTTONUP, flags, MAKELPARAM(point.x, point.y));
	if (!fullscreen_owner_ && ::GetCapture() == m_hWnd) ReleaseCapture();
}

BOOL VideoRenderWnd::OnMouseWheel(UINT flags, short delta, CPoint point)
{
	ForwardInput(WM_MOUSEWHEEL, MAKEWPARAM(flags, delta), MAKELPARAM(point.x, point.y));
	return TRUE;
}

int VideoRenderWnd::OnMouseActivate(CWnd*, UINT, UINT)
{
	SetFocus();
	if (fullscreen_owner_) SetCapture();
	return MA_ACTIVATE;
}

void VideoRenderWnd::OnActivate(UINT state, CWnd* other_window, BOOL minimized)
{
	CWnd::OnActivate(state, other_window, minimized);
	if (!fullscreen_owner_) return;
	if (state == WA_INACTIVE)
	{
		if (::GetCapture() == m_hWnd) ReleaseCapture();
	}
	else
	{
		SetFocus();
		SetCapture();
	}
}

void VideoRenderWnd::OnKeyDown(UINT key, UINT repeat, UINT flags)
{
	if (InputInjectorWin::IsInjectedMessage()) return;
	if (fullscreen_owner_ && (key == VK_ESCAPE || key == VK_F11))
	{
		fullscreen_owner_->ExitFullscreen();
		return;
	}
	if (key == VK_F10)
	{
		if (fullscreen_owner_) fullscreen_owner_->ToggleScaleMode();
		else ToggleScaleMode();
		return;
	}
	if ((key == VK_ESCAPE && fullscreen_) || key == VK_F11)
	{
		ToggleFullscreen();
		return;
	}
	if (input_callback_) input_callback_(this, WM_KEYDOWN, key, MAKELPARAM(repeat, flags));
}

void VideoRenderWnd::OnKeyUp(UINT key, UINT repeat, UINT flags)
{
	if (InputInjectorWin::IsInjectedMessage()) return;
	if (key == VK_ESCAPE || key == VK_F11 || key == VK_F10) return;
	if (input_callback_) input_callback_(this, WM_KEYUP, key, MAKELPARAM(repeat, flags));
}

void VideoRenderWnd::OnClose()
{
	if (fullscreen_owner_) fullscreen_owner_->ExitFullscreen();
	else if (fullscreen_) ExitFullscreen();
	else CWnd::OnClose();
}

void VideoRenderWnd::Submit(std::unique_ptr<VideoFrameData> frame)
{
	if (!frame || !GetSafeHwnd()) return;
	auto* raw = frame.release();
	if (!PostMessage(WM_VIDEO_FRAME, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
}

LRESULT VideoRenderWnd::OnFrame(WPARAM, LPARAM frame)
{
	std::unique_ptr<VideoFrameData> newest_frame(reinterpret_cast<VideoFrameData*>(frame));
	MSG pending{};
	while (::PeekMessageW(&pending, m_hWnd, WM_VIDEO_FRAME, WM_VIDEO_FRAME, PM_REMOVE))
		newest_frame.reset(reinterpret_cast<VideoFrameData*>(pending.lParam));
	if (fullscreen_view_ && ::IsWindow(fullscreen_view_->m_hWnd))
	{
		// Only one renderer is visible. Transfer the frame instead of copying a
		// multi-megabyte BGRA buffer on the UI thread for every video frame.
		fullscreen_view_->frame_ = std::move(newest_frame);
		fullscreen_view_->frame_dirty_ = true;
		fullscreen_view_->Invalidate(FALSE);
	}
	else
	{
		frame_ = std::move(newest_frame);
		frame_dirty_ = true;
		Invalidate(FALSE);
	}
	return 0;
}

BOOL VideoRenderWnd::OnEraseBkgnd(CDC*)
{
	// The swap chain owns every visible pixel. Suppressing WM_ERASEBKGND keeps
	// GDI from exposing a white or black frame before Present.
	return TRUE;
}

void VideoRenderWnd::OnDestroy()
{
	MSG pending{};
	while (::PeekMessageW(&pending, m_hWnd, WM_VIDEO_FRAME, WM_VIDEO_FRAME, PM_REMOVE))
		delete reinterpret_cast<VideoFrameData*>(pending.lParam);
	ReleaseD3d();
	CWnd::OnDestroy();
}

void VideoRenderWnd::OnSize(UINT type, int width, int height)
{
	CWnd::OnSize(type, width, height);
	if (type != SIZE_MINIMIZED && width > 0 && height > 0 && d3d_)
		ResizeD3d(width, height);
	Invalidate(FALSE);
}

void VideoRenderWnd::OnPaint()
{
	CPaintDC dc(this);
	if (!RenderD3d())
	{
		CRect client;
		GetClientRect(&client);
		dc.FillSolidRect(client, RGB(20, 20, 20));
	}
}

bool VideoRenderWnd::EnsureD3d(int width, int height)
{
	if (width <= 0 || height <= 0 || !::IsWindow(m_hWnd)) return false;
	if (d3d_)
	{
		if (d3d_->client_width != width || d3d_->client_height != height)
			return ResizeD3d(width, height);
		return true;
	}

	auto state = std::make_unique<D3dState>();
	DXGI_SWAP_CHAIN_DESC swap_description{};
	swap_description.BufferDesc.Width = static_cast<UINT>(width);
	swap_description.BufferDesc.Height = static_cast<UINT>(height);
	swap_description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swap_description.SampleDesc.Count = 1;
	swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_description.BufferCount = 2;
	swap_description.OutputWindow = m_hWnd;
	swap_description.Windowed = TRUE;
	// Flip-model presentation is the DWM-native path for a borderless D3D
	// window. It avoids the extra blit used by the legacy DISCARD model.
	swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	D3D_FEATURE_LEVEL feature_level{};
	HRESULT result = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &swap_description,
		&state->swap_chain, &state->device, &feature_level, &state->context);
	if (FAILED(result))
	{
		result = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &swap_description,
			&state->swap_chain, &state->device, &feature_level, &state->context);
	}
	if (FAILED(result)) return false;
	ComPtr<IDXGIDevice1> dxgi_device;
	if (SUCCEEDED(state->device.As(&dxgi_device))) dxgi_device->SetMaximumFrameLatency(1);

	ComPtr<ID3D11Texture2D> back_buffer;
	if (FAILED(state->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
		FAILED(state->device->CreateRenderTargetView(back_buffer.Get(), nullptr, &state->render_target))) return false;

	ComPtr<ID3DBlob> vertex_bytecode;
	ComPtr<ID3DBlob> pixel_bytecode;
	if (!CompileShader(kVertexShader, "vs_4_0", vertex_bytecode) ||
		!CompileShader(kPixelShader, "ps_4_0", pixel_bytecode)) return false;
	if (FAILED(state->device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
		vertex_bytecode->GetBufferSize(), nullptr, &state->vertex_shader)) ||
		FAILED(state->device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
			pixel_bytecode->GetBufferSize(), nullptr, &state->pixel_shader))) return false;

	D3D11_SAMPLER_DESC sampler_description{};
	// Desktop content contains small text and one-pixel edges. Point sampling
	// keeps those details crisp when the fullscreen viewport is scaled.
	sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(state->device->CreateSamplerState(&sampler_description, &state->sampler))) return false;

	state->client_width = width;
	state->client_height = height;
	d3d_ = std::move(state);
	frame_dirty_ = true;
	return true;
}

bool VideoRenderWnd::ResizeD3d(int width, int height)
{
	if (!d3d_ || width <= 0 || height <= 0) return false;
	if (d3d_->client_width == width && d3d_->client_height == height) return true;
	d3d_->context->OMSetRenderTargets(0, nullptr, nullptr);
	d3d_->render_target.Reset();
	if (FAILED(d3d_->swap_chain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
		DXGI_FORMAT_UNKNOWN, 0)))
	{
		ReleaseD3d();
		return false;
	}
	ComPtr<ID3D11Texture2D> back_buffer;
	if (FAILED(d3d_->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
		FAILED(d3d_->device->CreateRenderTargetView(back_buffer.Get(), nullptr, &d3d_->render_target)))
	{
		ReleaseD3d();
		return false;
	}
	d3d_->client_width = width;
	d3d_->client_height = height;
	return true;
}

bool VideoRenderWnd::RenderD3d()
{
	CRect client;
	GetClientRect(&client);
	if (client.IsRectEmpty() || !EnsureD3d(client.Width(), client.Height())) return false;

	if (frame_ && !frame_->pixels.empty() && frame_->width > 0 && frame_->height > 0 &&
		(frame_dirty_ || d3d_->texture_width != frame_->width || d3d_->texture_height != frame_->height))
	{
		if (d3d_->texture_width != frame_->width || d3d_->texture_height != frame_->height)
		{
			d3d_->video_view.Reset();
			d3d_->video_texture.Reset();
			D3D11_TEXTURE2D_DESC texture_description{};
			texture_description.Width = static_cast<UINT>(frame_->width);
			texture_description.Height = static_cast<UINT>(frame_->height);
			texture_description.MipLevels = 1;
			texture_description.ArraySize = 1;
			texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			texture_description.SampleDesc.Count = 1;
			texture_description.Usage = D3D11_USAGE_DYNAMIC;
			texture_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			texture_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(d3d_->device->CreateTexture2D(&texture_description, nullptr, &d3d_->video_texture)) ||
				FAILED(d3d_->device->CreateShaderResourceView(d3d_->video_texture.Get(), nullptr, &d3d_->video_view))) return false;
			d3d_->texture_width = frame_->width;
			d3d_->texture_height = frame_->height;
		}

		const size_t required_bytes = static_cast<size_t>(frame_->width) * frame_->height * 4;
		if (frame_->pixels.size() < required_bytes) return false;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(d3d_->context->Map(d3d_->video_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
		const size_t source_stride = static_cast<size_t>(frame_->width) * 4;
		for (int y = 0; y < frame_->height; ++y)
			std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
				frame_->pixels.data() + static_cast<size_t>(y) * source_stride, source_stride);
		d3d_->context->Unmap(d3d_->video_texture.Get(), 0);
		frame_dirty_ = false;
	}

	ID3D11RenderTargetView* render_target = d3d_->render_target.Get();
	d3d_->context->OMSetRenderTargets(1, &render_target, nullptr);
	const float clear_color[4] = {0.006f, 0.006f, 0.006f, 1.0f};
	d3d_->context->ClearRenderTargetView(render_target, clear_color);

	if (d3d_->video_view && frame_ && !frame_->pixels.empty())
	{
		const CRect video = CalculateVideoRect();
		D3D11_VIEWPORT viewport{};
		viewport.TopLeftX = static_cast<float>(video.left);
		viewport.TopLeftY = static_cast<float>(video.top);
		viewport.Width = static_cast<float>(video.Width());
		viewport.Height = static_cast<float>(video.Height());
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		d3d_->context->RSSetViewports(1, &viewport);
		d3d_->context->IASetInputLayout(nullptr);
		d3d_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		d3d_->context->VSSetShader(d3d_->vertex_shader.Get(), nullptr, 0);
		d3d_->context->PSSetShader(d3d_->pixel_shader.Get(), nullptr, 0);
		ID3D11ShaderResourceView* texture_view = d3d_->video_view.Get();
		ID3D11SamplerState* sampler = d3d_->sampler.Get();
		d3d_->context->PSSetShaderResources(0, 1, &texture_view);
		d3d_->context->PSSetSamplers(0, 1, &sampler);
		d3d_->context->Draw(4, 0);
		ID3D11ShaderResourceView* empty_view = nullptr;
		d3d_->context->PSSetShaderResources(0, 1, &empty_view);
	}

	// Never block the MFC UI thread waiting for a vertical blank. With a
	// flip-model swap chain DWM still composes the latest complete buffer; if
	// the queue is busy, drop this presentation and accept the next video frame.
	const HRESULT present_result = d3d_->swap_chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
	if (present_result == DXGI_ERROR_WAS_STILL_DRAWING) return true;
	if (present_result == DXGI_ERROR_DEVICE_REMOVED || present_result == DXGI_ERROR_DEVICE_RESET)
	{
		ReleaseD3d();
		return false;
	}
	return SUCCEEDED(present_result);
}

void VideoRenderWnd::ReleaseD3d()
{
	if (d3d_ && d3d_->context)
	{
		d3d_->context->ClearState();
		d3d_->context->Flush();
	}
	d3d_.reset();
}

CRect VideoRenderWnd::CalculateVideoRect()
{
	CRect client;
	GetClientRect(&client);
	if (client.IsRectEmpty() || !frame_ || frame_->width <= 0 || frame_->height <= 0)
		return CRect(0, 0, 0, 0);

	int width = frame_->width;
	int height = frame_->height;
	if (!original_size_)
	{
		width = client.Width();
		height = static_cast<int>(static_cast<long long>(width) * frame_->height / frame_->width);
		if (height > client.Height())
		{
			height = client.Height();
			width = static_cast<int>(static_cast<long long>(height) * frame_->width / frame_->height);
		}
	}
	width = (std::max)(1, width);
	height = (std::max)(1, height);
	const int left = client.left + (client.Width() - width) / 2;
	const int top = client.top + (client.Height() - height) / 2;
	return CRect(left, top, left + width, top + height);
}

bool VideoRenderWnd::NormalizeVideoPoint(CPoint point, unsigned short& normalized_x, unsigned short& normalized_y)
{
	CRect bounds;
	if (fullscreen_owner_ && !original_size_) GetClientRect(&bounds);
	else bounds = CalculateVideoRect();
	if (bounds.IsRectEmpty()) return false;
	const int x = (std::max)(bounds.left, (std::min)(point.x, bounds.right - 1)) - bounds.left;
	const int y = (std::max)(bounds.top, (std::min)(point.y, bounds.bottom - 1)) - bounds.top;
	normalized_x = static_cast<unsigned short>(MulDiv(x, 65535, (std::max)(1, bounds.Width() - 1)));
	normalized_y = static_cast<unsigned short>(MulDiv(y, 65535, (std::max)(1, bounds.Height() - 1)));
	return true;
}

void VideoRenderWnd::ToggleScaleMode()
{
	original_size_ = !original_size_;
	frame_dirty_ = true;
	Invalidate(FALSE);
	if (fullscreen_view_ && ::IsWindow(fullscreen_view_->m_hWnd))
	{
		fullscreen_view_->original_size_ = original_size_;
		fullscreen_view_->frame_dirty_ = true;
		fullscreen_view_->Invalidate(FALSE);
	}
	if (scale_mode_changed_callback_) scale_mode_changed_callback_(original_size_);
}

void VideoRenderWnd::ToggleFullscreen()
{
	if (fullscreen_) ExitFullscreen();
	else EnterFullscreen();
}

void VideoRenderWnd::EnterFullscreen()
{
	if (fullscreen_ || fullscreen_owner_ || !::IsWindow(m_hWnd)) return;

	MONITORINFO monitor_info{};
	monitor_info.cbSize = sizeof(monitor_info);
	const HMONITOR monitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
	if (!::GetMonitorInfoW(monitor, &monitor_info)) return;

	fullscreen_view_ = std::make_unique<VideoRenderWnd>();
	CWnd* owner = CWnd::FromHandle(::GetParent(m_hWnd));
	if (!owner || !fullscreen_view_->CreateFullscreen(monitor_info.rcMonitor, owner, this))
	{
		fullscreen_view_.reset();
		return;
	}
	fullscreen_view_->SetInputCallback(input_callback_);
	fullscreen_view_->original_size_ = original_size_;
	if (frame_)
	{
		fullscreen_view_->frame_ = std::make_unique<VideoFrameData>(*frame_);
		fullscreen_view_->frame_dirty_ = true;
	}
	// From this point the main-window renderer is dormant. New decoded frames
	// are posted directly to the fullscreen HWND by RemoteVideoRenderer.
	frame_.reset();
	frame_dirty_ = false;
	ReleaseD3d();
	fullscreen_ = true;
	::SetWindowPos(fullscreen_view_->m_hWnd, HWND_TOPMOST,
		monitor_info.rcMonitor.left, monitor_info.rcMonitor.top,
		monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
		monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
		SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	::SetForegroundWindow(fullscreen_view_->m_hWnd);
	::SetActiveWindow(fullscreen_view_->m_hWnd);
	fullscreen_view_->SetFocus();
	fullscreen_view_->SetCapture();
	fullscreen_view_->Invalidate(FALSE);
	if (fullscreen_changed_callback_) fullscreen_changed_callback_(true, fullscreen_view_->m_hWnd);
}

void VideoRenderWnd::ExitFullscreen()
{
	if (!fullscreen_ || !::IsWindow(m_hWnd)) return;
	fullscreen_ = false;
	// Switch the decoder's post target back before destroying the fullscreen
	// HWND. Any new frame from this point is queued on the main video window.
	if (fullscreen_changed_callback_) fullscreen_changed_callback_(false, m_hWnd);
	if (fullscreen_view_)
	{
		if (fullscreen_view_->frame_)
			frame_ = std::move(fullscreen_view_->frame_);
		fullscreen_view_->fullscreen_owner_ = nullptr;
		if (::IsWindow(fullscreen_view_->m_hWnd)) fullscreen_view_->DestroyWindow();
		fullscreen_view_.reset();
	}
	frame_dirty_ = true;
	SetFocus();
	Invalidate(FALSE);
	const HWND main_window = ::GetParent(m_hWnd);
	if (::IsWindow(main_window))
		::RedrawWindow(main_window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
}
