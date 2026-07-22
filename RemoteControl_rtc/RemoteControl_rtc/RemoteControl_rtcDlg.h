
// RemoteControl_rtcDlg.h: 头文件
//

#pragma once

#include <atomic>
#include <cstdint>

#include "VideoRenderWnd.h"

class SessionManager;
class SignalingClient;
class WebRtcEngine;
class DesktopCaptureManager;
class LocalAudioManager;
class PeerSession;
class RemoteVideoRecorder;
struct MediaOptions;

// CRemoteControlrtcDlg 对话框
class CRemoteControlrtcDlg : public CDialog
{
// 构造
public:
	CRemoteControlrtcDlg(CWnd* pParent = nullptr);	// 标准构造函数
	virtual ~CRemoteControlrtcDlg();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_REMOTECONTROL_RTC_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 支持


// 实现
protected:
	HICON m_hIcon;
	std::unique_ptr<WebRtcEngine> webrtc_engine_;
	std::unique_ptr<SessionManager> session_manager_;
	std::unique_ptr<SignalingClient> signaling_client_;
	std::unique_ptr<DesktopCaptureManager> desktop_capture_manager_;
	std::unique_ptr<LocalAudioManager> local_audio_manager_;
	std::unique_ptr<RemoteVideoRecorder> video_recorder_;
	CEdit server_edit_;
	CEdit port_edit_;
	CEdit user_id_edit_;
	CStatic server_label_;
	CStatic port_label_;
	CStatic user_id_label_;
	CStatic online_users_label_;
	CStatic log_label_;
	CButton connect_button_;
	CButton allow_control_check_;
	CButton send_video_check_;
	CButton receive_video_check_;
	CButton send_audio_check_;
	CButton receive_audio_check_;
	CButton hangup_button_;
	CButton record_button_;
	CButton fullscreen_button_;
	CButton scale_mode_button_;
	CButton keyboard_control_button_;
	CButton connect_user_button_;
	CStatic signaling_state_text_;
	CStatic ice_services_text_;
	CStatic media_stats_primary_text_;
	CStatic media_stats_network_text_;
	CListBox online_user_list_;
	CEdit log_edit_;
	VideoRenderWnd remote_video_view_;

	// 生成的消息映射函数
	virtual BOOL OnInitDialog();
	virtual BOOL PreTranslateMessage(MSG* message);
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnDestroy();
	afx_msg void OnTimer(UINT_PTR timer_id);
	afx_msg void OnBnClickedConnect();
	afx_msg void OnLbnDblclkOnlineUsers();
	afx_msg void OnBnClickedAllowControl();
	afx_msg void OnBnClickedHangup();
	afx_msg void OnBnClickedRecord();
	afx_msg void OnBnClickedFullscreen();
	afx_msg void OnBnClickedScaleMode();
	afx_msg void OnBnClickedKeyboardControl();
	afx_msg void OnBnClickedConnectUser();
	afx_msg LRESULT OnSignalingState(WPARAM state, LPARAM detail);
	afx_msg LRESULT OnSignalingMessage(WPARAM, LPARAM message);
	afx_msg LRESULT OnSessionState(WPARAM, LPARAM message);
	afx_msg LRESULT OnMediaEvent(WPARAM, LPARAM message);
	afx_msg LRESULT OnMediaStats(WPARAM, LPARAM message);
	afx_msg LRESULT OnControlEvent(WPARAM, LPARAM message);
	void CreateSignalingControls();
	void AppendLog(const CString& text);
	void DispatchSignalingMessage(const std::string& json);
	void BindSessionCallbacks(PeerSession* session, const std::string& remote_user_id);
	void SendRemoteInput(VideoRenderWnd* source, UINT message, WPARAM wparam, LPARAM lparam);
	void SetKeyboardControlActive(bool active);
	void ResetMediaStats();
	bool ConfigureLocalMedia(PeerSession* session, const MediaOptions& options);
	void StartSelectedSession();
	std::string active_remote_user_id_;
	bool keyboard_control_active_ = false;
	bool has_previous_media_stats_ = false;
	uint64_t stats_start_after_ms_ = 0;
	int64_t previous_stats_sample_time_us_ = 0;
	uint64_t previous_video_bytes_received_ = 0;
	uint64_t previous_audio_bytes_received_ = 0;
	uint64_t previous_video_packets_received_ = 0;
	uint64_t previous_audio_packets_received_ = 0;
	int64_t previous_video_packets_lost_ = 0;
	int64_t previous_audio_packets_lost_ = 0;
	uint64_t previous_video_frames_decoded_ = 0;
	double previous_video_jitter_buffer_delay_seconds_ = 0.0;
	uint64_t previous_video_jitter_buffer_emitted_count_ = 0;
	std::atomic<bool> shutting_down_ = false;
	DECLARE_MESSAGE_MAP()
};
