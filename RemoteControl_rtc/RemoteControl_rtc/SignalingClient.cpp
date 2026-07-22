#include "pch.h"
#include "SignalingClient.h"

#include <ws2tcpip.h>

SignalingClient::SignalingClient() = default;
SignalingClient::~SignalingClient() { Disconnect(); }

void SignalingClient::SetStateCallback(StateCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	state_callback_ = std::move(callback);
}

void SignalingClient::SetMessageCallback(MessageCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	message_callback_ = std::move(callback);
}

bool SignalingClient::Connect(const std::wstring& host, unsigned short port, const std::wstring& user_id)
{
	if (host.empty() || user_id.empty() || port == 0) return false;
	Disconnect();
	stop_requested_ = false;
	worker_ = std::thread(&SignalingClient::Run, this, host, port, user_id);
	return true;
}

void SignalingClient::Disconnect()
{
	stop_requested_ = true;
	SOCKET socket = INVALID_SOCKET;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		socket = socket_;
	}
	if (socket != INVALID_SOCKET) shutdown(socket, SD_BOTH);
	if (worker_.joinable()) worker_.join();
	connected_ = false;
}

bool SignalingClient::Send(const std::string& json_line)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!connected_ || socket_ == INVALID_SOCKET) return false;
	const std::string payload = json_line + "\n";
	size_t sent_total = 0;
	while (sent_total < payload.size())
	{
		const int sent = send(socket_, payload.data() + sent_total, static_cast<int>(payload.size() - sent_total), 0);
		if (sent <= 0) return false;
		sent_total += static_cast<size_t>(sent);
	}
	return true;
}

void SignalingClient::Run(std::wstring host, unsigned short port, std::wstring user_id)
{
	ReportState(SignalingConnectionState::Connecting, L"Connecting to signaling server...");
	addrinfoW hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfoW* addresses = nullptr;
	const std::wstring service = std::to_wstring(port);
	if (GetAddrInfoW(host.c_str(), service.c_str(), &hints, &addresses) != 0)
	{
		ReportState(SignalingConnectionState::Failed, L"Unable to resolve the signaling server.");
		return;
	}

	SOCKET connected_socket = INVALID_SOCKET;
	for (addrinfoW* address = addresses; address != nullptr && !stop_requested_; address = address->ai_next)
	{
		SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (candidate != INVALID_SOCKET && connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0)
		{
			connected_socket = candidate;
			break;
		}
		if (candidate != INVALID_SOCKET) closesocket(candidate);
	}
	FreeAddrInfoW(addresses);
	if (connected_socket == INVALID_SOCKET)
	{
		if (!stop_requested_) ReportState(SignalingConnectionState::Failed, L"Unable to connect to the signaling server.");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		socket_ = connected_socket;
	}
	connected_ = true;
	DWORD receive_timeout_ms = 1000;
	setsockopt(connected_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout_ms), sizeof(receive_timeout_ms));
	const std::string login = "{\"type\":\"login\",\"userId\":\"" + EscapeJson(ToUtf8(user_id)) + "\"}";
	if (!Send(login))
		ReportState(SignalingConnectionState::Failed, L"Failed to send the login message.");
	else
		ReportState(SignalingConnectionState::Connected, L"Connected; login request sent.");

	std::string pending;
	char buffer[4096];
	ULONGLONG last_heartbeat = GetTickCount64();
	while (!stop_requested_ && connected_)
	{
		const int received = recv(connected_socket, buffer, static_cast<int>(sizeof(buffer)), 0);
		if (received == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT)
		{
			if (GetTickCount64() - last_heartbeat >= 15000)
			{
				if (!Send("{\"type\":\"ping\"}")) break;
				last_heartbeat = GetTickCount64();
			}
			continue;
		}
		if (received <= 0) break;
		pending.append(buffer, received);
		size_t newline = 0;
		while ((newline = pending.find('\n')) != std::string::npos)
		{
			std::string message = pending.substr(0, newline);
			pending.erase(0, newline + 1);
			MessageCallback callback;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				callback = message_callback_;
			}
			if (callback && !message.empty()) callback(message);
		}
	}

	connected_ = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (socket_ == connected_socket) socket_ = INVALID_SOCKET;
	}
	closesocket(connected_socket);
	if (!stop_requested_) ReportState(SignalingConnectionState::Disconnected, L"The signaling connection was closed.");
}

void SignalingClient::ReportState(SignalingConnectionState state, const std::wstring& detail)
{
	StateCallback callback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		callback = state_callback_;
	}
	if (callback) callback(state, detail);
}

std::string SignalingClient::ToUtf8(const std::wstring& value)
{
	if (value.empty()) return {};
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	std::string result(bytes, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], bytes, nullptr, nullptr);
	return result;
}

std::string SignalingClient::EscapeJson(const std::string& value)
{
	std::string result;
	for (char ch : value)
	{
		if (ch == '\\' || ch == '"') result += '\\';
		result += ch;
	}
	return result;
}
