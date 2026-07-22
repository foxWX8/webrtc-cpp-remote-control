#pragma once

#include <functional>
#include <memory>
#include <vector>

struct VideoFrameData
{
	int width = 0;
	int height = 0;
	std::vector<unsigned char> pixels;
};

class VideoRenderWnd : public CWnd
{
public:
	using InputCallback = std::function<void(VideoRenderWnd*, UINT, WPARAM, LPARAM)>;
	using FullscreenChangedCallback = std::function<void(bool, HWND)>;
	using ScaleModeChangedCallback = std::function<void(bool)>;
	VideoRenderWnd();
	~VideoRenderWnd() override;
	BOOL Create(const RECT& rect, CWnd* parent, UINT id);
	BOOL CreateFullscreen(const RECT& rect, CWnd* owner, VideoRenderWnd* fullscreen_owner);
	void Submit(std::unique_ptr<VideoFrameData> frame);
	void Clear() { Submit(std::make_unique<VideoFrameData>()); }
	void SetInputCallback(InputCallback callback);
	void SetFullscreenChangedCallback(FullscreenChangedCallback callback) { fullscreen_changed_callback_ = std::move(callback); }
	void SetScaleModeChangedCallback(ScaleModeChangedCallback callback) { scale_mode_changed_callback_ = std::move(callback); }
	void ToggleFullscreen();
	void ExitFullscreen();
	void ToggleScaleMode();
	bool IsFullscreen() const { return fullscreen_; }
	bool IsOriginalSize() const { return original_size_; }
	bool NormalizeVideoPoint(CPoint point, unsigned short& normalized_x, unsigned short& normalized_y);

protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC* dc);
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT type, int width, int height);
	afx_msg LRESULT OnFrame(WPARAM, LPARAM frame);
	afx_msg void OnMouseMove(UINT flags, CPoint point);
	afx_msg void OnLButtonDown(UINT flags, CPoint point);
	afx_msg void OnLButtonDblClk(UINT flags, CPoint point);
	afx_msg void OnLButtonUp(UINT flags, CPoint point);
	afx_msg void OnRButtonDown(UINT flags, CPoint point);
	afx_msg void OnRButtonUp(UINT flags, CPoint point);
	afx_msg void OnMButtonDown(UINT flags, CPoint point);
	afx_msg void OnMButtonUp(UINT flags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
	afx_msg int OnMouseActivate(CWnd* desktop_window, UINT hit_test, UINT message);
	afx_msg void OnActivate(UINT state, CWnd* other_window, BOOL minimized);
	afx_msg void OnKeyDown(UINT key, UINT repeat, UINT flags);
	afx_msg void OnKeyUp(UINT key, UINT repeat, UINT flags);
	afx_msg void OnClose();
	DECLARE_MESSAGE_MAP()

private:
	struct D3dState;
	CRect CalculateVideoRect();
	bool EnsureD3d(int width, int height);
	bool ResizeD3d(int width, int height);
	bool RenderD3d();
	void ReleaseD3d();
	void EnterFullscreen();
	void ForwardInput(UINT message, WPARAM wparam, LPARAM lparam);
	std::unique_ptr<D3dState> d3d_;
	std::unique_ptr<VideoRenderWnd> fullscreen_view_;
	std::unique_ptr<VideoFrameData> frame_;
	bool frame_dirty_ = false;
	InputCallback input_callback_;
	FullscreenChangedCallback fullscreen_changed_callback_;
	ScaleModeChangedCallback scale_mode_changed_callback_;
	bool fullscreen_ = false;
	bool original_size_ = false;
	VideoRenderWnd* fullscreen_owner_ = nullptr;
};
