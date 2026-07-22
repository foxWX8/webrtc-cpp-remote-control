
// RemoteControl_rtcDlg.cpp: 实现文件
//

#include "pch.h"
#include "framework.h"
#include "RemoteControl_rtc.h"
#include "RemoteControl_rtcDlg.h"
#include "afxdialogex.h"

#include "SessionManager.h"
#include "SignalingClient.h"
#include "SignalingProtocol.h"
#include "PeerSession.h"
#include "DesktopCaptureManager.h"
#include "RemoteControlChannel.h"
#include "LocalAudioManager.h"
#include "RemoteVideoRecorder.h"
#include "WebRtcEngine.h"

namespace
{
constexpr UINT IDC_CONNECT_SIGNALING = 2001;
constexpr UINT IDC_ONLINE_USERS = 2006;
constexpr UINT IDC_ALLOW_CONTROL = 2008;
constexpr UINT IDC_HANGUP = 2009;
constexpr UINT IDC_RECORD = 2016;
constexpr UINT IDC_CONNECT_USER = 2017;
constexpr UINT IDC_FULLSCREEN = 2018;
constexpr UINT IDC_KEYBOARD_CONTROL = 2019;
constexpr UINT IDC_VIDEO_SCALE_MODE = 2027;
constexpr UINT WM_SIGNALING_STATE = WM_APP + 101;
constexpr UINT WM_SIGNALING_MESSAGE = WM_APP + 102;
constexpr UINT WM_SESSION_STATE = WM_APP + 103;
constexpr UINT WM_MEDIA_EVENT = WM_APP + 104;
constexpr UINT WM_MEDIA_STATS = WM_APP + 105;
constexpr UINT WM_CONTROL_EVENT = WM_APP + 106;
constexpr UINT_PTR MEDIA_STATS_TIMER_ID = 1;

struct SessionStateMessage
{
	std::string remote_user_id;
	PeerSessionState state = PeerSessionState::New;
};

struct MediaEventMessage
{
	std::string remote_user_id;
	PeerMediaEvent event = PeerMediaEvent::RemoteVideoTrackAdded;
	int width = 0;
	int height = 0;
};

struct MediaStatsMessage
{
	std::string remote_user_id;
	PeerConnectionStats stats;
};

struct ControlEventMessage
{
	std::string remote_user_id;
	std::string detail;
};

CString FormatBitrate(double bits_per_second)
{
	CString text;
	if (bits_per_second >= 1000000.0) text.Format(L"%.2f Mbps", bits_per_second / 1000000.0);
	else if (bits_per_second >= 1000.0) text.Format(L"%.0f kbps", bits_per_second / 1000.0);
	else text.Format(L"%.0f bps", bits_per_second);
	return text;
}

double IntervalLossPercent(uint64_t received, int64_t lost,
	uint64_t previous_received, int64_t previous_lost)
{
	const uint64_t received_delta = received >= previous_received ? received - previous_received : 0;
	const int64_t raw_lost_delta = lost >= previous_lost ? lost - previous_lost : 0;
	const uint64_t lost_delta = raw_lost_delta > 0 ? static_cast<uint64_t>(raw_lost_delta) : 0;
	const uint64_t total = received_delta + lost_delta;
	return total ? static_cast<double>(lost_delta) * 100.0 / static_cast<double>(total) : 0.0;
}

CString IceRouteText(const std::string& value)
{
	CString route;
	if (value.rfind("turn-relay", 0) == 0) route = L"TURN 中继";
	else if (value.rfind("stun-direct", 0) == 0) route = L"STUN 穿透直连";
	else if (value.rfind("host-direct", 0) == 0) route = L"局域网直连";
	else return L"等待 ICE 选路";
	const auto separator = value.find('/');
	if (separator != std::string::npos && separator + 1 < value.size())
	{
		CString protocol(CA2W(value.substr(separator + 1).c_str(), CP_UTF8));
		protocol.MakeUpper();
		route += L" / " + protocol;
	}
	return route;
}
}

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// 用于应用程序“关于”菜单项的 CAboutDlg 对话框

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持

// 实现
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


// CRemoteControlrtcDlg 对话框



CRemoteControlrtcDlg::CRemoteControlrtcDlg(CWnd* pParent /*=nullptr*/)
	: CDialog(IDD_REMOTECONTROL_RTC_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

CRemoteControlrtcDlg::~CRemoteControlrtcDlg() = default;

void CRemoteControlrtcDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CRemoteControlrtcDlg, CDialog)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_DESTROY()
	ON_WM_TIMER()
	ON_BN_CLICKED(IDC_CONNECT_SIGNALING, &CRemoteControlrtcDlg::OnBnClickedConnect)
	ON_LBN_DBLCLK(IDC_ONLINE_USERS, &CRemoteControlrtcDlg::OnLbnDblclkOnlineUsers)
	ON_BN_CLICKED(IDC_ALLOW_CONTROL, &CRemoteControlrtcDlg::OnBnClickedAllowControl)
	ON_BN_CLICKED(IDC_HANGUP, &CRemoteControlrtcDlg::OnBnClickedHangup)
	ON_BN_CLICKED(IDC_RECORD, &CRemoteControlrtcDlg::OnBnClickedRecord)
	ON_BN_CLICKED(IDC_FULLSCREEN, &CRemoteControlrtcDlg::OnBnClickedFullscreen)
	ON_BN_CLICKED(IDC_VIDEO_SCALE_MODE, &CRemoteControlrtcDlg::OnBnClickedScaleMode)
	ON_BN_CLICKED(IDC_KEYBOARD_CONTROL, &CRemoteControlrtcDlg::OnBnClickedKeyboardControl)
	ON_BN_CLICKED(IDC_CONNECT_USER, &CRemoteControlrtcDlg::OnBnClickedConnectUser)
	ON_MESSAGE(WM_SIGNALING_STATE, &CRemoteControlrtcDlg::OnSignalingState)
	ON_MESSAGE(WM_SIGNALING_MESSAGE, &CRemoteControlrtcDlg::OnSignalingMessage)
	ON_MESSAGE(WM_SESSION_STATE, &CRemoteControlrtcDlg::OnSessionState)
	ON_MESSAGE(WM_MEDIA_EVENT, &CRemoteControlrtcDlg::OnMediaEvent)
	ON_MESSAGE(WM_MEDIA_STATS, &CRemoteControlrtcDlg::OnMediaStats)
	ON_MESSAGE(WM_CONTROL_EVENT, &CRemoteControlrtcDlg::OnControlEvent)
END_MESSAGE_MAP()


// CRemoteControlrtcDlg 消息处理程序

BOOL CRemoteControlrtcDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// 将“关于...”菜单项添加到系统菜单中。

	// IDM_ABOUTBOX 必须在系统命令范围内。
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// 设置此对话框的图标。  当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	ModifyStyle(0, WS_MINIMIZEBOX, SWP_FRAMECHANGED);
	SetWindowPos(nullptr, 0, 0, 1100, 650, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	CenterWindow();
	SetWindowText(L"RemoteControl RTC");
	// Capture the complete controlled desktop, including this application's UI.
	// Same-machine tests may show a recursive preview, which is expected when
	// no application window is excluded from DXGI Desktop Duplication.
	::SetWindowDisplayAffinity(m_hWnd, WDA_NONE);
	// 隐藏 MFC 模板生成的占位文本和默认确定/取消按钮。
	if (auto* control = GetDlgItem(IDOK)) control->ShowWindow(SW_HIDE);
	if (auto* control = GetDlgItem(IDCANCEL)) control->ShowWindow(SW_HIDE);
	if (auto* control = GetDlgItem(IDC_STATIC)) control->ShowWindow(SW_HIDE);
	CreateSignalingControls();

	webrtc_engine_ = std::make_unique<WebRtcEngine>();
	if (!webrtc_engine_->Initialize())
	{
		AfxMessageBox(L"无法初始化 libwebrtc。请确认 x64 的 libwebrtc.dll 已放在程序目录中。", MB_ICONERROR);
		return FALSE;
	}
	session_manager_ = std::make_unique<SessionManager>(*webrtc_engine_);
	desktop_capture_manager_ = std::make_unique<DesktopCaptureManager>();
	local_audio_manager_ = std::make_unique<LocalAudioManager>();
	video_recorder_ = std::make_unique<RemoteVideoRecorder>();
	signaling_client_ = std::make_unique<SignalingClient>();
	const HWND dialog_window = m_hWnd;
	signaling_client_->SetStateCallback([dialog_window](SignalingConnectionState state, const std::wstring& detail)
		{
			auto* message = new std::wstring(detail);
			if (!::IsWindow(dialog_window) ||
				!::PostMessageW(dialog_window, WM_SIGNALING_STATE, static_cast<WPARAM>(state), reinterpret_cast<LPARAM>(message)))
			{
				delete message;
			}
		});
	signaling_client_->SetMessageCallback([dialog_window](const std::string& message)
		{
			auto* copy = new std::string(message);
			if (!::IsWindow(dialog_window) ||
				!::PostMessageW(dialog_window, WM_SIGNALING_MESSAGE, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
		});
	AppendLog(L"WebRTC 已初始化。请输入信令服务器信息后连接。");
	SetTimer(MEDIA_STATS_TIMER_ID, 1000, nullptr);

	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

BOOL CRemoteControlrtcDlg::PreTranslateMessage(MSG* message)
{
	if (message && (message->message == WM_KEYDOWN || message->message == WM_KEYUP))
	{
		const bool escape_fullscreen = message->wParam == VK_ESCAPE && remote_video_view_.IsFullscreen();
		const bool toggle_fullscreen = message->wParam == VK_F11;
		if (escape_fullscreen || toggle_fullscreen)
		{
			if (message->message == WM_KEYDOWN) remote_video_view_.ToggleFullscreen();
			return TRUE;
		}
	}
	return CDialog::PreTranslateMessage(message);
}

void CRemoteControlrtcDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialog::OnSysCommand(nID, lParam);
	}
}

// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。  对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void CRemoteControlrtcDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作区矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标
//显示。
HCURSOR CRemoteControlrtcDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CRemoteControlrtcDlg::OnDestroy()
{
	shutting_down_ = true;
	KillTimer(MEDIA_STATS_TIMER_ID);
	if (signaling_client_)
	{
		signaling_client_->SetStateCallback({});
		signaling_client_->SetMessageCallback({});
		signaling_client_->Disconnect();
		signaling_client_.reset();
	}
	// PeerConnection 必须先于 libwebrtc 的全局线程销毁。
	session_manager_.reset();
	video_recorder_.reset();
	desktop_capture_manager_.reset();
	local_audio_manager_.reset();
	webrtc_engine_.reset();
	remote_video_view_.ExitFullscreen();

	// These controls are created dynamically and are CWnd members. Destroy or
	// detach them while their parent HWND is still valid, so their destructors
	// never retain a stale child handle after Windows tears down the dialog.
	auto release_child = [](CWnd& child)
		{
			const HWND window = child.GetSafeHwnd();
			if (!window) return;
			if (::IsWindow(window)) child.DestroyWindow();
			else child.Detach();
		};
	release_child(remote_video_view_);
	release_child(log_edit_);
	release_child(online_user_list_);
	release_child(media_stats_network_text_);
	release_child(media_stats_primary_text_);
	release_child(ice_services_text_);
	release_child(signaling_state_text_);
	release_child(connect_user_button_);
	release_child(record_button_);
	release_child(fullscreen_button_);
	release_child(scale_mode_button_);
	release_child(keyboard_control_button_);
	release_child(hangup_button_);
	release_child(receive_audio_check_);
	release_child(send_audio_check_);
	release_child(receive_video_check_);
	release_child(send_video_check_);
	release_child(allow_control_check_);
	release_child(connect_button_);
	release_child(log_label_);
	release_child(online_users_label_);
	release_child(user_id_label_);
	release_child(port_label_);
	release_child(server_label_);
	release_child(user_id_edit_);
	release_child(port_edit_);
	release_child(server_edit_);
	CDialog::OnDestroy();
}

void CRemoteControlrtcDlg::OnTimer(UINT_PTR timer_id)
{
	if (timer_id == MEDIA_STATS_TIMER_ID && !shutting_down_ && session_manager_ &&
		!active_remote_user_id_.empty() && stats_start_after_ms_ != 0 &&
		::GetTickCount64() >= stats_start_after_ms_)
	{
		auto* session = session_manager_->FindSession(active_remote_user_id_);
		if (session && session->State() == PeerSessionState::Connected)
		{
			auto* control_channel = session->ControlChannel();
			if (control_channel && control_channel->IsOpen()) control_channel->SendPing();
			const HWND dialog_window = m_hWnd;
			session->RequestStats([dialog_window](const std::string& user_id, const PeerConnectionStats& stats)
				{
					auto* message = new MediaStatsMessage{ user_id, stats };
					if (!::IsWindow(dialog_window) || !::PostMessageW(dialog_window, WM_MEDIA_STATS, 0,
						reinterpret_cast<LPARAM>(message))) delete message;
				});
		}
	}
	CDialog::OnTimer(timer_id);
}

void CRemoteControlrtcDlg::CreateSignalingControls()
{
	const DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
	CFont* font = GetFont();
	server_label_.Create(L"服务器", WS_CHILD | WS_VISIBLE, CRect(22, 17, 70, 37), this, 2010);
	server_edit_.Create(edit_style, CRect(76, 12, 265, 37), this, 2002);
	port_label_.Create(L"端口", WS_CHILD | WS_VISIBLE, CRect(276, 17, 318, 37), this, 2011);
	port_edit_.Create(edit_style | ES_NUMBER, CRect(318, 12, 383, 37), this, 2003);
	user_id_label_.Create(L"用户 ID", WS_CHILD | WS_VISIBLE, CRect(395, 17, 440, 37), this, 2012);
	user_id_edit_.Create(edit_style, CRect(445, 12, 590, 37), this, 2004);
	connect_button_.Create(L"连接", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(600, 12, 670, 37), this, IDC_CONNECT_SIGNALING);
	allow_control_check_.Create(L"允许远端控制本机", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(685, 15, 850, 38), this, IDC_ALLOW_CONTROL);
	hangup_button_.Create(L"挂断", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(860, 12, 930, 37), this, IDC_HANGUP);
	record_button_.Create(L"开始录制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(940, 12, 1025, 37), this, IDC_RECORD);
	fullscreen_button_.Create(L"全屏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(1030, 12, 1080, 37), this, IDC_FULLSCREEN);
	scale_mode_button_.Create(L"原始尺寸", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		CRect(820, 45, 930, 72), this, IDC_VIDEO_SCALE_MODE);
	keyboard_control_button_.Create(L"启动键鼠控制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		CRect(940, 45, 1080, 72), this, IDC_KEYBOARD_CONTROL);
	send_video_check_.Create(L"发送屏幕", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(22, 48, 115, 70), this, 2020);
	receive_video_check_.Create(L"接收视频", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(125, 48, 218, 70), this, 2021);
	send_audio_check_.Create(L"发送麦克风", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(228, 48, 335, 70), this, 2022);
	receive_audio_check_.Create(L"接收音频", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(345, 48, 438, 70), this, 2023);
	send_video_check_.SetCheck(BST_CHECKED);
	receive_video_check_.SetCheck(BST_CHECKED);
	signaling_state_text_.Create(L"未连接", WS_CHILD | WS_VISIBLE, CRect(455, 50, 810, 72), this, 2005);
	online_users_label_.Create(L"在线用户", WS_CHILD | WS_VISIBLE, CRect(22, 85, 105, 107), this, 2013);
	connect_user_button_.Create(L"连接所选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(125, 80, 210, 105), this, IDC_CONNECT_USER);
	online_user_list_.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
		CRect(22, 110, 210, 520), this, IDC_ONLINE_USERS);
	log_label_.Create(L"信令日志", WS_CHILD | WS_VISIBLE, CRect(225, 85, 525, 107), this, 2014);
	log_edit_.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		CRect(225, 110, 525, 520), this, 2007);
	remote_video_view_.Create(CRect(540, 85, 1060, 520), this, 2015);
	ice_services_text_.Create(L"ICE 服务：正在读取配置…", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
		CRect(22, 530, 1078, 550), this, 2024);
	media_stats_primary_text_.Create(L"视频：尚未建立 WebRTC 连接", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
		CRect(22, 554, 1078, 574), this, 2025);
	media_stats_network_text_.Create(L"网络：等待媒体统计", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
		CRect(22, 578, 1078, 600), this, 2026);
	remote_video_view_.SetInputCallback([this](VideoRenderWnd* source, UINT message, WPARAM wparam, LPARAM lparam)
		{ SendRemoteInput(source, message, wparam, lparam); });
	remote_video_view_.SetFullscreenChangedCallback([this](bool fullscreen, HWND render_target)
		{
			fullscreen_button_.SetWindowText(fullscreen ? L"还原" : L"全屏");
			if (shutting_down_ || !session_manager_ || active_remote_user_id_.empty()) return;
			auto* session = session_manager_->FindSession(active_remote_user_id_);
			if (session) session->UpdateRemoteVideoTarget(render_target);
		});
	remote_video_view_.SetScaleModeChangedCallback([this](bool original_size)
		{
			scale_mode_button_.SetWindowText(original_size ? L"适应窗口" : L"原始尺寸");
		});
	for (CWnd* control : { static_cast<CWnd*>(&server_edit_), static_cast<CWnd*>(&port_edit_), static_cast<CWnd*>(&user_id_edit_),
		static_cast<CWnd*>(&server_label_), static_cast<CWnd*>(&port_label_), static_cast<CWnd*>(&user_id_label_), static_cast<CWnd*>(&online_users_label_), static_cast<CWnd*>(&log_label_),
		static_cast<CWnd*>(&connect_button_), static_cast<CWnd*>(&allow_control_check_), static_cast<CWnd*>(&hangup_button_), static_cast<CWnd*>(&record_button_), static_cast<CWnd*>(&fullscreen_button_), static_cast<CWnd*>(&scale_mode_button_), static_cast<CWnd*>(&keyboard_control_button_), static_cast<CWnd*>(&connect_user_button_),
		static_cast<CWnd*>(&send_video_check_), static_cast<CWnd*>(&receive_video_check_), static_cast<CWnd*>(&send_audio_check_), static_cast<CWnd*>(&receive_audio_check_),
		static_cast<CWnd*>(&signaling_state_text_), static_cast<CWnd*>(&ice_services_text_), static_cast<CWnd*>(&media_stats_primary_text_), static_cast<CWnd*>(&media_stats_network_text_),
		static_cast<CWnd*>(&online_user_list_), static_cast<CWnd*>(&log_edit_) })
	{
		control->SetFont(font);
	}
	const CString ice_summary(CA2W(PeerSession::IceServiceSummary().c_str(), CP_UTF8));
	ice_services_text_.SetWindowText(L"ICE 服务：" + ice_summary);
	server_edit_.SetCueBanner(L"信令服务器地址，例如 150.158.3.4");
	port_edit_.SetCueBanner(L"端口");
	user_id_edit_.SetCueBanner(L"用户 ID");
	server_edit_.SetWindowText(L"150.158.3.4");
	port_edit_.SetWindowText(L"8000");
	user_id_edit_.SetWindowText(L"user-001");
}

void CRemoteControlrtcDlg::OnBnClickedConnect()
{
	// Keep this button useful after a media hangup. The signaling TCP session
	// stays online, so a click now starts a new WebRTC session with the selected
	// user instead of attempting a second signaling login.
	if (signaling_client_ && signaling_client_->IsConnected())
	{
		StartSelectedSession();
		return;
	}

	CString server;
	CString port_text;
	CString user_id;
	server_edit_.GetWindowText(server);
	port_edit_.GetWindowText(port_text);
	user_id_edit_.GetWindowText(user_id);
	const unsigned long port = wcstoul(port_text, nullptr, 10);
	if (server.IsEmpty() || user_id.IsEmpty() || port == 0 || port > 65535)
	{
		AfxMessageBox(L"请输入有效的服务器地址、端口和用户 ID。", MB_ICONWARNING);
		return;
	}
	connect_button_.EnableWindow(FALSE);
	if (!signaling_client_->Connect(server.GetString(), static_cast<unsigned short>(port), user_id.GetString()))
	{
		connect_button_.EnableWindow(TRUE);
		AfxMessageBox(L"无法启动信令连接。", MB_ICONERROR);
	}
}

LRESULT CRemoteControlrtcDlg::OnSignalingState(WPARAM state, LPARAM detail)
{
	std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(detail));
	CString display_text(text ? text->c_str() : L"");
	signaling_state_text_.SetWindowText(display_text);
	AppendLog(display_text);
	const auto connection_state = static_cast<SignalingConnectionState>(state);
	if (connection_state == SignalingConnectionState::Connected)
	{
		connect_button_.SetWindowText(L"连接用户");
		connect_button_.EnableWindow(TRUE);
	}
	else
	{
		connect_button_.SetWindowText(connection_state == SignalingConnectionState::Connecting ?
			L"连接中…" : L"连接");
		connect_button_.EnableWindow(connection_state == SignalingConnectionState::Disconnected ||
			connection_state == SignalingConnectionState::Failed);
	}
	return 0;
}

void CRemoteControlrtcDlg::AppendLog(const CString& text)
{
	if (!::IsWindow(log_edit_.GetSafeHwnd())) return;
	const int length = log_edit_.GetWindowTextLength();
	log_edit_.SetSel(length, length);
	CString line;
	if (length > 0) line = L"\r\n";
	line += text;
	log_edit_.ReplaceSel(line, FALSE);
	log_edit_.LineScroll(log_edit_.GetLineCount());
}

void CRemoteControlrtcDlg::OnLbnDblclkOnlineUsers()
{
	StartSelectedSession();
}

void CRemoteControlrtcDlg::OnBnClickedConnectUser()
{
	StartSelectedSession();
}

void CRemoteControlrtcDlg::StartSelectedSession()
{
	const int selected = online_user_list_.GetCurSel();
	if (selected == LB_ERR)
	{
		AppendLog(L"请先选择一个在线用户。");
		return;
	}
	CString user_id;
	online_user_list_.GetText(selected, user_id);
	AppendLog(L"准备连接用户：" + user_id);
	UpdateWindow();
	// CStringA's second argument is not a code page. Passing CP_UTF8 there is
	// interpreted as an internal string-manager pointer and causes an access
	// violation before PeerConnection creation. Use ATL's conversion helper.
	CW2A utf8(user_id.GetString(), CP_UTF8);
	const std::string remote_user_id(utf8.m_psz ? utf8.m_psz : "");
	if (remote_user_id != active_remote_user_id_)
	{
		SetKeyboardControlActive(false);
		ResetMediaStats();
	}
	active_remote_user_id_ = remote_user_id;
	MediaOptions options;
	options.send_video = send_video_check_.GetCheck() == BST_CHECKED;
	options.receive_video = receive_video_check_.GetCheck() == BST_CHECKED;
	options.send_audio = send_audio_check_.GetCheck() == BST_CHECKED;
	options.receive_audio = receive_audio_check_.GetCheck() == BST_CHECKED;
	options.enable_remote_control = true;
	AppendLog(L"步骤 1/4：创建 PeerConnection…");
	UpdateWindow();
	PeerSession* session = session_manager_->CreateSession(remote_user_id, options);
	if (!session)
	{
		AppendLog(L"该用户已有会话，或会话创建失败。");
		return;
	}
	if (options.enable_remote_control && !session->CreateControlChannel())
	{
		AppendLog(L"无法创建远程控制 DataChannel。");
		session_manager_->RemoveSession(remote_user_id);
		return;
	}
	AppendLog(L"步骤 2/4：配置本地媒体…");
	UpdateWindow();
	if (!ConfigureLocalMedia(session, options))
	{
		session_manager_->RemoveSession(remote_user_id);
		return;
	}
	AppendLog(L"步骤 3/4：绑定视频、录制和控制通道…");
	UpdateWindow();
	session->SetRemoteVideoTarget(remote_video_view_.GetSafeHwnd());
	session->SetRecorder(video_recorder_.get());
	session->SetRemoteControlAuthorized(allow_control_check_.GetCheck() == BST_CHECKED);
	BindSessionCallbacks(session, remote_user_id);
	AppendLog(L"步骤 4/4：创建并发送 WebRTC Offer…");
	UpdateWindow();
	session->CreateOffer([this, remote_user_id](const std::string& sdp, const std::string&)
		{ signaling_client_->Send(SignalingProtocol::Offer(remote_user_id, sdp)); });
}

LRESULT CRemoteControlrtcDlg::OnSignalingMessage(WPARAM, LPARAM message)
{
	std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(message));
	if (json) DispatchSignalingMessage(*json);
	return 0;
}

void CRemoteControlrtcDlg::DispatchSignalingMessage(const std::string& json)
{
	const std::string type = SignalingProtocol::StringField(json, "type");
	if (type == "presence.snapshot")
	{
		online_user_list_.ResetContent();
		for (const auto& user_id : SignalingProtocol::UserIds(json))
			online_user_list_.AddString(CString(CA2W(user_id.c_str(), CP_UTF8)));
		AppendLog(L"在线用户列表已更新。");
		return;
	}
	if (type == "presence.changed")
	{
		const std::string user_id = SignalingProtocol::StringField(json, "userId");
		if (user_id.empty()) return;
		const CString display(CA2W(user_id.c_str(), CP_UTF8));
		const int existing = online_user_list_.FindStringExact(-1, display);
		if (SignalingProtocol::BoolField(json, "online"))
		{
			if (existing == LB_ERR) online_user_list_.AddString(display);
		}
		else if (existing != LB_ERR)
		{
			online_user_list_.DeleteString(existing);
		}
		AppendLog(SignalingProtocol::BoolField(json, "online") ? L"有用户上线。" : L"有用户下线。");
		return;
	}

	const std::string remote_user_id = SignalingProtocol::StringField(json, "fromUserId");
	if (remote_user_id.empty()) return;
	if (remote_user_id != active_remote_user_id_)
	{
		SetKeyboardControlActive(false);
		ResetMediaStats();
	}
	active_remote_user_id_ = remote_user_id;
	if (type == "offer")
	{
		MediaOptions options;
		options.send_video = send_video_check_.GetCheck() == BST_CHECKED;
		options.receive_video = receive_video_check_.GetCheck() == BST_CHECKED;
		options.send_audio = send_audio_check_.GetCheck() == BST_CHECKED;
		options.receive_audio = receive_audio_check_.GetCheck() == BST_CHECKED;
		options.enable_remote_control = true;
		auto* session = session_manager_->FindSession(remote_user_id);
		if (!session) session = session_manager_->CreateSession(remote_user_id, options);
		if (!session) return;
		if (!ConfigureLocalMedia(session, options)) { session_manager_->RemoveSession(remote_user_id); return; }
		session->SetRemoteVideoTarget(remote_video_view_.GetSafeHwnd());
		session->SetRecorder(video_recorder_.get());
		session->SetRemoteControlAuthorized(allow_control_check_.GetCheck() == BST_CHECKED);
		BindSessionCallbacks(session, remote_user_id);
		session->ApplyRemoteOffer(SignalingProtocol::StringField(json, "sdp"), [this, remote_user_id](const std::string& sdp, const std::string&)
			{ signaling_client_->Send(SignalingProtocol::Answer(remote_user_id, sdp)); });
		AppendLog(L"已收到 Offer，正在创建 Answer。");
	}
	else if (type == "answer")
	{
		if (auto* session = session_manager_->FindSession(remote_user_id)) session->ApplyRemoteAnswer(SignalingProtocol::StringField(json, "sdp"));
	}
	else if (type == "ice-candidate")
	{
		if (auto* session = session_manager_->FindSession(remote_user_id))
			session->AddRemoteCandidate(SignalingProtocol::StringField(json, "mid"), SignalingProtocol::IntField(json, "mlineIndex"), SignalingProtocol::StringField(json, "candidate"));
	}
	else if (type == "hangup")
	{
		session_manager_->RemoveSession(remote_user_id);
		if (video_recorder_->IsRecording()) video_recorder_->Stop();
		record_button_.SetWindowText(L"开始录制");
		remote_video_view_.Clear();
		active_remote_user_id_.clear();
		ResetMediaStats();
		signaling_state_text_.SetWindowText(L"信令已连接；当前无活动会话");
		if (session_manager_->SessionCount() == 0)
		{
			desktop_capture_manager_->Stop();
			local_audio_manager_->Stop();
		}
		connect_button_.SetWindowText(L"连接用户");
		connect_button_.EnableWindow(TRUE);
		connect_user_button_.EnableWindow(TRUE);
		AppendLog(L"远端已挂断会话。");
	}
}

void CRemoteControlrtcDlg::BindSessionCallbacks(PeerSession* session, const std::string& remote_user_id)
{
	if (!session) return;
	const HWND dialog_window = m_hWnd;
	session->SetStateChangedCallback([this, dialog_window](const std::string& user_id, PeerSessionState state)
		{
			if (shutting_down_) return;
			auto* message = new SessionStateMessage{user_id, state};
			if (!::IsWindow(dialog_window) ||
				!::PostMessageW(dialog_window, WM_SESSION_STATE, 0, reinterpret_cast<LPARAM>(message))) delete message;
		});
	session->SetIceCandidateCallback([this, remote_user_id](const std::string& mid, int index, const std::string& candidate)
		{
			if (!shutting_down_ && signaling_client_)
				signaling_client_->Send(SignalingProtocol::Candidate(remote_user_id, mid, index, candidate));
		});
	session->SetMediaEventCallback([this, dialog_window](const std::string& user_id, PeerMediaEvent event, int width, int height)
		{
			if (shutting_down_) return;
			auto* message = new MediaEventMessage{user_id, event, width, height};
			if (!::IsWindow(dialog_window) ||
				!::PostMessageW(dialog_window, WM_MEDIA_EVENT, 0, reinterpret_cast<LPARAM>(message))) delete message;
		});
	session->SetControlEventCallback([this, dialog_window](const std::string& user_id, const std::string& detail)
		{
			if (shutting_down_) return;
			auto* message = new ControlEventMessage{user_id, detail};
			if (!::IsWindow(dialog_window) ||
				!::PostMessageW(dialog_window, WM_CONTROL_EVENT, 0, reinterpret_cast<LPARAM>(message))) delete message;
		});
}

LRESULT CRemoteControlrtcDlg::OnControlEvent(WPARAM, LPARAM value)
{
	std::unique_ptr<ControlEventMessage> message(reinterpret_cast<ControlEventMessage*>(value));
	if (!message) return 0;
	const CString user(CA2W(message->remote_user_id.c_str(), CP_UTF8));
	const CString detail(CA2W(message->detail.c_str(), CP_UTF8));
	CString text;
	text.Format(L"鼠标调试 [%s]：%s", user.GetString(), detail.GetString());
	AppendLog(text);
	return 0;
}

LRESULT CRemoteControlrtcDlg::OnSessionState(WPARAM, LPARAM value)
{
	std::unique_ptr<SessionStateMessage> message(reinterpret_cast<SessionStateMessage*>(value));
	if (!message) return 0;
	const CString user(CA2W(message->remote_user_id.c_str(), CP_UTF8));
	CString text;
	switch (message->state)
	{
	case PeerSessionState::Connecting: text = L"正在建立 WebRTC 连接：" + user; break;
	case PeerSessionState::Connected: text = L"WebRTC 已连接：" + user; break;
	case PeerSessionState::Disconnected: text = L"WebRTC 连接已中断：" + user; break;
	case PeerSessionState::Failed: text = L"WebRTC 协商或连接失败：" + user; break;
	case PeerSessionState::Closed: text = L"WebRTC 会话已关闭：" + user; break;
	default: return 0;
	}
	AppendLog(text);
	if (message->remote_user_id == active_remote_user_id_)
	{
		signaling_state_text_.SetWindowText(text);
		if (message->state == PeerSessionState::Connected)
			stats_start_after_ms_ = ::GetTickCount64() + 2000;
		if (message->state == PeerSessionState::Disconnected ||
			message->state == PeerSessionState::Failed || message->state == PeerSessionState::Closed)
		{
			SetKeyboardControlActive(false);
			ResetMediaStats();
		}
	}
	if (message->state == PeerSessionState::Failed || message->state == PeerSessionState::Closed)
	{
		// A delayed Closed notification from an old call must not erase a new
		// session to the same user. Only remove the session that is still in the
		// terminal state represented by this message.
		auto* current_session = session_manager_ ?
			session_manager_->FindSession(message->remote_user_id) : nullptr;
		if (current_session && current_session->State() == message->state)
		{
			session_manager_->RemoveSession(message->remote_user_id);
			if (message->remote_user_id == active_remote_user_id_)
				active_remote_user_id_.clear();
			if (session_manager_->SessionCount() == 0)
			{
				desktop_capture_manager_->Stop();
				local_audio_manager_->Stop();
			}
		}
		if (signaling_client_ && signaling_client_->IsConnected())
		{
			connect_button_.SetWindowText(L"连接用户");
			connect_button_.EnableWindow(TRUE);
			connect_user_button_.EnableWindow(TRUE);
		}
	}
	return 0;
}

LRESULT CRemoteControlrtcDlg::OnMediaEvent(WPARAM, LPARAM value)
{
	std::unique_ptr<MediaEventMessage> message(reinterpret_cast<MediaEventMessage*>(value));
	if (!message) return 0;
	const CString user(CA2W(message->remote_user_id.c_str(), CP_UTF8));
	CString text;
	switch (message->event)
	{
	case PeerMediaEvent::RemoteVideoTrackAdded:
		text = L"远端视频轨道已绑定，等待首帧：" + user;
		break;
	case PeerMediaEvent::RemoteVideoFirstFrame:
		text.Format(L"已收到远端视频首帧：%s（%d × %d）", user.GetString(), message->width, message->height);
		break;
	case PeerMediaEvent::RemoteVideoFrameConversionFailed:
		text.Format(L"远端视频帧转换失败：%s（%d × %d）", user.GetString(), message->width, message->height);
		break;
	default:
		return 0;
	}
	AppendLog(text);
	return 0;
}

LRESULT CRemoteControlrtcDlg::OnMediaStats(WPARAM, LPARAM value)
{
	std::unique_ptr<MediaStatsMessage> message(reinterpret_cast<MediaStatsMessage*>(value));
	if (!message || message->remote_user_id != active_remote_user_id_) return 0;
	const PeerConnectionStats& stats = message->stats;
	const double elapsed_seconds = has_previous_media_stats_ &&
		stats.sample_time_us > previous_stats_sample_time_us_
		? static_cast<double>(stats.sample_time_us - previous_stats_sample_time_us_) / 1000000.0 : 0.0;
	const bool valid_interval = elapsed_seconds >= 0.1 && elapsed_seconds <= 10.0;

	const auto bitrate = [elapsed_seconds, valid_interval](uint64_t current, uint64_t previous)
		{
			return valid_interval && current >= previous
				? static_cast<double>(current - previous) * 8.0 / elapsed_seconds : 0.0;
		};
	const double video_bitrate = bitrate(stats.video_bytes_received, previous_video_bytes_received_);
	const double audio_bitrate = bitrate(stats.audio_bytes_received, previous_audio_bytes_received_);
	const double video_loss = valid_interval ? IntervalLossPercent(stats.video_packets_received,
		stats.video_packets_lost, previous_video_packets_received_, previous_video_packets_lost_) : 0.0;
	const double audio_loss = valid_interval ? IntervalLossPercent(stats.audio_packets_received,
		stats.audio_packets_lost, previous_audio_packets_received_, previous_audio_packets_lost_) : 0.0;
	double frames_per_second = stats.video_frames_per_second;
	if (frames_per_second <= 0.0 && valid_interval && stats.video_frames_decoded >= previous_video_frames_decoded_)
		frames_per_second = static_cast<double>(stats.video_frames_decoded - previous_video_frames_decoded_) / elapsed_seconds;

	double video_buffer_ms = 0.0;
	bool video_buffer_interval_available = false;
	if (stats.video_buffer_available && valid_interval &&
		stats.video_jitter_buffer_delay_seconds >= previous_video_jitter_buffer_delay_seconds_ &&
		stats.video_jitter_buffer_emitted_count > previous_video_jitter_buffer_emitted_count_)
	{
		video_buffer_ms = (stats.video_jitter_buffer_delay_seconds - previous_video_jitter_buffer_delay_seconds_) * 1000.0 /
			static_cast<double>(stats.video_jitter_buffer_emitted_count - previous_video_jitter_buffer_emitted_count_);
		video_buffer_interval_available = true;
	}
	else if (stats.video_buffer_available && stats.video_playout_delay_ms > 0.0)
	{
		video_buffer_ms = stats.video_playout_delay_ms;
		video_buffer_interval_available = true;
	}

	CString resolution = L"--";
	if (stats.video_width > 0 && stats.video_height > 0)
		resolution.Format(L"%d × %d", stats.video_width, stats.video_height);
	CString fps = L"--";
	if (frames_per_second > 0.0) fps.Format(L"%.1f FPS", frames_per_second);
	const CString video_rate = stats.video_stats_available && valid_interval ?
		FormatBitrate(video_bitrate) : CString(L"采样中");
	CString video_loss_text = L"--";
	if (stats.video_stats_available && valid_interval) video_loss_text.Format(L"%.2f%%", video_loss);
	CString video_buffer_text = L"--";
	if (video_buffer_interval_available) video_buffer_text.Format(L"%.0f ms", video_buffer_ms);

	CString video_text;
	if (stats.video_stats_available)
	{
		video_text.Format(L"视频：%s  %s  |  接收 %s  |  包 %I64u / 丢包 %s  |  播放缓冲 %s",
			resolution.GetString(), fps.GetString(), video_rate.GetString(), stats.video_packets_received,
			video_loss_text.GetString(), video_buffer_text.GetString());
	}
	else
	{
		video_text = L"视频：等待接收视频统计";
	}
	media_stats_primary_text_.SetWindowText(video_text);

	CString rtt = L"--";
	if (stats.round_trip_time_available)
	{
		rtt.Format(L"%.0f ms", stats.round_trip_time_ms);
	}
	else if (session_manager_)
	{
		auto* session = session_manager_->FindSession(active_remote_user_id_);
		auto* control_channel = session ? session->ControlChannel() : nullptr;
		if (control_channel && control_channel->RoundTripTimeMs() > 0)
			rtt.Format(L"%u ms", control_channel->RoundTripTimeMs());
	}
	CString video_jitter = L"--";
	if (stats.video_jitter_available) video_jitter.Format(L"%.1f ms", stats.video_jitter_ms);
	const CString incoming_rate = stats.available_incoming_bitrate_available ?
		FormatBitrate(stats.available_incoming_bitrate_bps) : CString(L"--");
	CString audio_summary = L"未接收/暂无统计";
	if (stats.audio_stats_available)
	{
		CString audio_loss_text = L"--";
		if (valid_interval) audio_loss_text.Format(L"%.2f%%", audio_loss);
		CString audio_jitter = L"--";
		if (stats.audio_jitter_available) audio_jitter.Format(L"%.1f ms", stats.audio_jitter_ms);
		const CString audio_rate = valid_interval ? FormatBitrate(audio_bitrate) : CString(L"采样中");
		audio_summary.Format(L"%s / 包 %I64u / 丢包 %s / 抖动 %s", audio_rate.GetString(),
			stats.audio_packets_received, audio_loss_text.GetString(), audio_jitter.GetString());
	}
	CString network_text;
	network_text.Format(L"网络：RTT %s  |  下行 %s  |  视频抖动 %s  |  %s  |  音频 %s",
		rtt.GetString(), incoming_rate.GetString(), video_jitter.GetString(),
		IceRouteText(stats.ice_route).GetString(), audio_summary.GetString());
	media_stats_network_text_.SetWindowText(network_text);

	has_previous_media_stats_ = true;
	previous_stats_sample_time_us_ = stats.sample_time_us;
	previous_video_bytes_received_ = stats.video_bytes_received;
	previous_audio_bytes_received_ = stats.audio_bytes_received;
	previous_video_packets_received_ = stats.video_packets_received;
	previous_audio_packets_received_ = stats.audio_packets_received;
	previous_video_packets_lost_ = stats.video_packets_lost;
	previous_audio_packets_lost_ = stats.audio_packets_lost;
	previous_video_frames_decoded_ = stats.video_frames_decoded;
	previous_video_jitter_buffer_delay_seconds_ = stats.video_jitter_buffer_delay_seconds;
	previous_video_jitter_buffer_emitted_count_ = stats.video_jitter_buffer_emitted_count;
	return 0;
}

void CRemoteControlrtcDlg::ResetMediaStats()
{
	has_previous_media_stats_ = false;
	stats_start_after_ms_ = 0;
	previous_stats_sample_time_us_ = 0;
	previous_video_bytes_received_ = 0;
	previous_audio_bytes_received_ = 0;
	previous_video_packets_received_ = 0;
	previous_audio_packets_received_ = 0;
	previous_video_packets_lost_ = 0;
	previous_audio_packets_lost_ = 0;
	previous_video_frames_decoded_ = 0;
	previous_video_jitter_buffer_delay_seconds_ = 0.0;
	previous_video_jitter_buffer_emitted_count_ = 0;
	if (::IsWindow(media_stats_primary_text_.GetSafeHwnd()))
		media_stats_primary_text_.SetWindowText(L"视频：尚未建立 WebRTC 连接");
	if (::IsWindow(media_stats_network_text_.GetSafeHwnd()))
		media_stats_network_text_.SetWindowText(L"网络：等待媒体统计");
}

void CRemoteControlrtcDlg::SendRemoteInput(VideoRenderWnd* source, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (!keyboard_control_active_) return;
	if (!session_manager_ || active_remote_user_id_.empty()) return;
	auto* session = session_manager_->FindSession(active_remote_user_id_);
	if (!session || !session->ControlChannel()) return;
	auto* channel = session->ControlChannel();
	if (message == WM_KEYDOWN || message == WM_KEYUP)
	{
		channel->SendKey(static_cast<unsigned short>(wparam), message == WM_KEYDOWN);
		return;
	}
	if (message == WM_MOUSEWHEEL)
	{
		const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
		channel->SendMouseWheel(delta);
		CString text;
		text.Format(L"发送鼠标滚轮：delta=%d", delta);
		AppendLog(text);
		return;
	}
	if (message == WM_MOUSEMOVE)
	{
		unsigned short normalized_x = 0;
		unsigned short normalized_y = 0;
		if (source && source->NormalizeVideoPoint(
			CPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), normalized_x, normalized_y))
			channel->SendMouseMove(normalized_x, normalized_y);
		return;
	}
	unsigned char button = 0;
	if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) button = 1;
	else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) button = 2;
	const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
	unsigned short normalized_x = 0;
	unsigned short normalized_y = 0;
	if (source && source->NormalizeVideoPoint(
		CPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), normalized_x, normalized_y))
	{
		channel->SendMouseButton(button, down, normalized_x, normalized_y);
		const wchar_t* button_name = button == 0 ? L"左键" : (button == 1 ? L"右键" : L"中键");
		CString text;
		text.Format(L"发送鼠标%s%s：x=%u y=%u，协议=%s", button_name, down ? L"按下" : L"抬起",
			normalized_x, normalized_y, channel->UsesSynchronizedMouse() ? L"同步" : L"兼容");
		AppendLog(text);
	}
}

void CRemoteControlrtcDlg::OnBnClickedAllowControl()
{
	const bool authorized = allow_control_check_.GetCheck() == BST_CHECKED;
	if (session_manager_) session_manager_->SetRemoteControlAuthorized(authorized);
	AppendLog(authorized ? L"已允许远端控制本机。" : L"已禁止远端控制本机。");
}

bool CRemoteControlrtcDlg::ConfigureLocalMedia(PeerSession* session, const MediaOptions& options)
{
	if (!session) return false;
	if (options.send_video)
	{
		if (!desktop_capture_manager_->VideoTrack() && !desktop_capture_manager_->StartPrimaryScreen(webrtc_engine_->Factory()))
		{ AppendLog(L"无法启动主屏幕采集。"); return false; }
		if (!session->AddLocalVideoTrack(desktop_capture_manager_->VideoTrack()))
		{ AppendLog(L"无法将屏幕视频加入会话。"); return false; }
	}
	if (options.send_audio)
	{
		if (!local_audio_manager_->AudioTrack() && !local_audio_manager_->Start(webrtc_engine_->Factory()))
		{ AppendLog(L"无法启动麦克风。"); return false; }
		if (!session->AddLocalAudioTrack(local_audio_manager_->AudioTrack()))
		{ AppendLog(L"无法将麦克风音频加入会话。"); return false; }
	}
	return true;
}

void CRemoteControlrtcDlg::OnBnClickedHangup()
{
	SetKeyboardControlActive(false);
	if (!session_manager_ || active_remote_user_id_.empty())
	{
		AppendLog(L"当前没有可挂断的活动会话。");
		return;
	}
	if (!session_manager_->FindSession(active_remote_user_id_))
	{
		AppendLog(L"活动会话已经结束。");
		active_remote_user_id_.clear();
		return;
	}
	signaling_client_->Send(SignalingProtocol::Hangup(active_remote_user_id_));
	session_manager_->RemoveSession(active_remote_user_id_);
	if (video_recorder_->IsRecording()) video_recorder_->Stop();
	record_button_.SetWindowText(L"开始录制");
	active_remote_user_id_.clear();
	remote_video_view_.Clear();
	ResetMediaStats();
	signaling_state_text_.SetWindowText(L"信令已连接；当前无活动会话");
	if (session_manager_->SessionCount() == 0)
	{
		desktop_capture_manager_->Stop();
		local_audio_manager_->Stop();
	}
	if (signaling_client_ && signaling_client_->IsConnected())
	{
		connect_button_.SetWindowText(L"连接用户");
		connect_button_.EnableWindow(TRUE);
		connect_user_button_.EnableWindow(TRUE);
	}
	AppendLog(L"会话已挂断。");
}

void CRemoteControlrtcDlg::OnBnClickedFullscreen()
{
	remote_video_view_.ToggleFullscreen();
}

void CRemoteControlrtcDlg::OnBnClickedScaleMode()
{
	remote_video_view_.ToggleScaleMode();
	AppendLog(remote_video_view_.IsOriginalSize() ?
		L"视频显示已切换为原始尺寸（全屏中可按 F10 切换）。" :
		L"视频显示已切换为适应窗口（全屏中可按 F10 切换）。");
}

void CRemoteControlrtcDlg::OnBnClickedKeyboardControl()
{
	if (keyboard_control_active_)
	{
		SetKeyboardControlActive(false);
		AppendLog(L"已停止向远端发送键盘和鼠标操作。");
		return;
	}

	if (!session_manager_ || active_remote_user_id_.empty())
	{
		AppendLog(L"请先连接远端用户，再启动键鼠控制。");
		return;
	}
	auto* session = session_manager_->FindSession(active_remote_user_id_);
	if (!session || !session->ControlChannel() || !session->ControlChannel()->IsOpen())
	{
		AppendLog(L"远程控制通道尚未就绪，请等待 WebRTC 连接完成。");
		return;
	}

	SetKeyboardControlActive(true);
	remote_video_view_.SetFocus();
	AppendLog(L"键鼠控制已启动；视频窗口获得焦点后，操作将发送到远端。");
}

void CRemoteControlrtcDlg::SetKeyboardControlActive(bool active)
{
	keyboard_control_active_ = active;
	if (::IsWindow(keyboard_control_button_.GetSafeHwnd()))
		keyboard_control_button_.SetWindowText(active ? L"停止键鼠控制" : L"启动键鼠控制");
}

void CRemoteControlrtcDlg::OnBnClickedRecord()
{
	if (video_recorder_->IsRecording())
	{
		video_recorder_->Stop();
		record_button_.SetWindowText(L"开始录制");
		AppendLog(L"录制已停止，MP4 文件已收尾。");
		return;
	}
	if (active_remote_user_id_.empty() || !session_manager_->FindSession(active_remote_user_id_))
	{
		AfxMessageBox(L"请先连接并拉取远端视频。", MB_ICONINFORMATION);
		return;
	}
	const CString default_name = CTime::GetCurrentTime().Format(L"remote_%Y%m%d_%H%M%S.mp4");
	CFileDialog dialog(FALSE, L"mp4", default_name, OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
		L"MP4 视频文件 (*.mp4)|*.mp4||", this);
	if (dialog.DoModal() != IDOK) return;
	if (!video_recorder_->Start(dialog.GetPathName().GetString()))
	{
		AfxMessageBox(L"无法启动录制。", MB_ICONERROR);
		return;
	}
	record_button_.SetWindowText(L"停止录制");
	AppendLog(L"MP4/H.264 录制已启动，等待远端视频帧（主界面和全屏均持续录制）。");
}

