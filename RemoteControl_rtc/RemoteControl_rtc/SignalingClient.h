#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

enum class SignalingConnectionState : unsigned int
{
	Disconnected,
	Connecting,
	Connected,
	Failed,
};

// TCP signaling transport. Messages use UTF-8 JSON, one object per line.
// This keeps signaling independent from WebRTC and easy to inspect with tools.
class SignalingClient final
{
public:
	using StateCallback = std::function<void(SignalingConnectionState, const std::wstring&)>;
	using MessageCallback = std::function<void(const std::string&)>;

	SignalingClient();
	~SignalingClient();

	SignalingClient(const SignalingClient&) = delete;
	SignalingClient& operator=(const SignalingClient&) = delete;

	void SetStateCallback(StateCallback callback);
	void SetMessageCallback(MessageCallback callback);
	bool Connect(const std::wstring& host, unsigned short port, const std::wstring& user_id);
	void Disconnect();
	bool Send(const std::string& json_line);
	bool IsConnected() const { return connected_.load(); }

private:
	void Run(std::wstring host, unsigned short port, std::wstring user_id);
	void ReportState(SignalingConnectionState state, const std::wstring& detail);
	static std::string ToUtf8(const std::wstring& value);
	static std::string EscapeJson(const std::string& value);

	std::atomic<bool> stop_requested_ = false;
	std::atomic<bool> connected_ = false;
	std::mutex mutex_;
	SOCKET socket_ = INVALID_SOCKET;
	std::thread worker_;
	StateCallback state_callback_;
	MessageCallback message_callback_;
};
