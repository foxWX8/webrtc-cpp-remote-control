#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr std::size_t kReadChunkSize = 16 * 1024;
constexpr std::size_t kMaxMessageSize = 4 * 1024 * 1024;
constexpr std::size_t kMaxPendingSendBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxClients = FD_SETSIZE > 32 ? FD_SETSIZE - 16 : 16;

struct Client
{
	Socket socket = kInvalidSocket;
	std::string user_id;
	std::string receive_buffer;
	std::string send_buffer;
};

std::atomic<bool> g_stopping = false;
Socket g_listen_socket = kInvalidSocket;
std::unordered_map<Socket, std::unique_ptr<Client>> g_clients;
std::unordered_map<std::string, Socket> g_users;

void Log(const std::string& text)
{
	std::cout << text << std::endl;
}

void CloseSocket(Socket socket)
{
	if (socket == kInvalidSocket) return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
}

bool SetNonBlocking(Socket socket)
{
#ifdef _WIN32
	u_long enabled = 1;
	return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
	const int flags = fcntl(socket, F_GETFL, 0);
	return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool WouldBlock()
{
#ifdef _WIN32
	const int error = WSAGetLastError();
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

int SendFlags()
{
#ifdef MSG_NOSIGNAL
	return MSG_NOSIGNAL;
#else
	return 0;
#endif
}

std::string EscapeJson(const std::string& value)
{
	std::string result;
	result.reserve(value.size());
	for (const unsigned char ch : value)
	{
		switch (ch)
		{
		case '\\': result += "\\\\"; break;
		case '"': result += "\\\""; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (ch >= 0x20) result += static_cast<char>(ch);
			break;
		}
	}
	return result;
}

std::size_t FindValue(const std::string& json, const char* name)
{
	const std::string key = std::string("\"") + name + "\"";
	std::size_t position = json.find(key);
	if (position == std::string::npos) return position;
	position = json.find(':', position + key.size());
	if (position == std::string::npos) return position;
	return json.find_first_not_of(" \t\r\n", position + 1);
}

std::string StringField(const std::string& json, const char* name)
{
	std::size_t position = FindValue(json, name);
	if (position == std::string::npos || json[position] != '"') return {};
	std::string result;
	for (++position; position < json.size(); ++position)
	{
		if (json[position] == '"') return result;
		if (json[position] == '\\' && ++position < json.size())
		{
			switch (json[position])
			{
			case 'n': result += '\n'; break;
			case 'r': result += '\r'; break;
			case 't': result += '\t'; break;
			case '\\': result += '\\'; break;
			case '"': result += '"'; break;
			default: return {};
			}
		}
		else result += json[position];
		if (result.size() > 1024) return {};
	}
	return {};
}

bool QueueLine(Client& client, const std::string& json)
{
	if (json.size() + 1 > kMaxPendingSendBytes - client.send_buffer.size()) return false;
	client.send_buffer += json;
	client.send_buffer.push_back('\n');
	return true;
}

std::string BuildPresenceSnapshot(const std::string& current_user)
{
	std::string json = "{\"type\":\"presence.snapshot\",\"users\":[";
	bool first = true;
	for (const auto& entry : g_users)
	{
		if (entry.first == current_user) continue;
		if (!first) json.push_back(',');
		first = false;
		json += "{\"userId\":\"" + EscapeJson(entry.first) + "\"}";
	}
	json += "]}";
	return json;
}

void BroadcastPresence(const std::string& user_id, bool online)
{
	const std::string message = "{\"type\":\"presence.changed\",\"userId\":\"" +
		EscapeJson(user_id) + "\",\"online\":" + (online ? "true" : "false") + "}";
	for (auto& entry : g_clients)
	{
		if (!entry.second->user_id.empty() && entry.second->user_id != user_id)
			QueueLine(*entry.second, message);
	}
}

std::string AddSender(const std::string& json, const std::string& sender)
{
	const std::size_t end = json.rfind('}');
	if (end == std::string::npos) return {};
	std::string routed = json;
	routed.insert(end, ",\"fromUserId\":\"" + EscapeJson(sender) + "\"");
	return routed;
}

bool RegisterClient(Client& client, const std::string& user_id)
{
	if (user_id.empty() || user_id.size() > 128 || g_users.find(user_id) != g_users.end()) return false;
	for (const unsigned char ch : user_id)
		if (ch < 0x20 || ch == '"' || ch == '\\') return false;
	client.user_id = user_id;
	g_users.emplace(user_id, client.socket);
	if (!QueueLine(client, BuildPresenceSnapshot(user_id))) return false;
	BroadcastPresence(user_id, true);
	Log("online: " + user_id);
	return true;
}

void RouteMessage(Client& sender, const std::string& json)
{
	const std::string type = StringField(json, "type");
	if (type == "ping")
	{
		QueueLine(sender, "{\"type\":\"pong\"}");
		return;
	}
	if (type == "login")
	{
		if (!sender.user_id.empty() || !RegisterClient(sender, StringField(json, "userId")))
			QueueLine(sender, "{\"type\":\"error\",\"code\":\"login_failed\"}");
		return;
	}
	if (sender.user_id.empty())
	{
		QueueLine(sender, "{\"type\":\"error\",\"code\":\"not_logged_in\"}");
		return;
	}
	if (type != "offer" && type != "answer" && type != "ice-candidate" && type != "hangup")
	{
		QueueLine(sender, "{\"type\":\"error\",\"code\":\"unsupported_message\"}");
		return;
	}
	if (!StringField(json, "fromUserId").empty())
	{
		QueueLine(sender, "{\"type\":\"error\",\"code\":\"sender_field_forbidden\"}");
		return;
	}
	const std::string target_id = StringField(json, "toUserId");
	const auto user = g_users.find(target_id);
	const auto target = user == g_users.end() ? g_clients.end() : g_clients.find(user->second);
	if (target == g_clients.end())
	{
		QueueLine(sender, "{\"type\":\"error\",\"code\":\"target_offline\",\"userId\":\"" +
			EscapeJson(target_id) + "\"}");
		return;
	}
	const std::string routed = AddSender(json, sender.user_id);
	if (routed.empty() || !QueueLine(*target->second, routed))
		QueueLine(sender, "{\"type\":\"error\",\"code\":\"route_failed\"}");
}

void DisconnectClient(Socket socket)
{
	const auto item = g_clients.find(socket);
	if (item == g_clients.end()) return;
	const std::string user_id = item->second->user_id;
	if (!user_id.empty())
	{
		const auto user = g_users.find(user_id);
		if (user != g_users.end() && user->second == socket) g_users.erase(user);
	}
	CloseSocket(socket);
	g_clients.erase(item);
	if (!user_id.empty())
	{
		Log("offline: " + user_id);
		BroadcastPresence(user_id, false);
	}
}

Socket CreateListenSocket(const char* bind_address, const char* port)
{
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;
	addrinfo* addresses = nullptr;
	if (getaddrinfo(bind_address, port, &hints, &addresses) != 0) return kInvalidSocket;
	Socket listen_socket = kInvalidSocket;
	for (addrinfo* address = addresses; address; address = address->ai_next)
	{
		listen_socket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (listen_socket == kInvalidSocket) continue;
		int reuse = 1;
		setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
			reinterpret_cast<const char*>(&reuse), sizeof(reuse));
		if (bind(listen_socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0 &&
			listen(listen_socket, SOMAXCONN) == 0 && SetNonBlocking(listen_socket)) break;
		CloseSocket(listen_socket);
		listen_socket = kInvalidSocket;
	}
	freeaddrinfo(addresses);
	return listen_socket;
}

void AcceptClients()
{
	for (;;)
	{
		Socket socket = accept(g_listen_socket, nullptr, nullptr);
		if (socket == kInvalidSocket)
		{
			if (!WouldBlock()) Log("accept failed");
			return;
		}
		if (g_clients.size() >= kMaxClients || !SetNonBlocking(socket)
#ifndef _WIN32
			|| socket >= FD_SETSIZE
#endif
			)
		{
			CloseSocket(socket);
			continue;
		}
		auto client = std::make_unique<Client>();
		client->socket = socket;
		g_clients.emplace(socket, std::move(client));
	}
}

bool ReadClient(Client& client)
{
	char buffer[kReadChunkSize];
	for (;;)
	{
		const int received = recv(client.socket, buffer, static_cast<int>(sizeof(buffer)), 0);
		if (received == 0) return false;
		if (received < 0) return WouldBlock();
		client.receive_buffer.append(buffer, static_cast<std::size_t>(received));
		if (client.receive_buffer.size() > kMaxMessageSize) return false;
		for (;;)
		{
			const std::size_t newline = client.receive_buffer.find('\n');
			if (newline == std::string::npos) break;
			std::string message = client.receive_buffer.substr(0, newline);
			client.receive_buffer.erase(0, newline + 1);
			if (!message.empty() && message.back() == '\r') message.pop_back();
			if (!message.empty()) RouteMessage(client, message);
		}
	}
}

bool WriteClient(Client& client)
{
	while (!client.send_buffer.empty())
	{
		const std::size_t count = (std::min)(client.send_buffer.size(),
			static_cast<std::size_t>(0x7fffffff));
		const int sent = send(client.socket, client.send_buffer.data(), static_cast<int>(count), SendFlags());
		if (sent < 0) return WouldBlock();
		if (sent == 0) return false;
		client.send_buffer.erase(0, static_cast<std::size_t>(sent));
	}
	return true;
}

#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD event)
{
	if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT || event == CTRL_BREAK_EVENT)
	{
		g_stopping = true;
		return TRUE;
	}
	return FALSE;
}
#else
void SignalHandler(int)
{
	g_stopping = true;
}
#endif
}

int main(int argc, char** argv)
{
	const char* bind_address = argc > 1 ? argv[1] : "0.0.0.0";
	const char* port = argc > 2 ? argv[2] : "8000";
#ifdef _WIN32
	WSADATA data{};
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);
	std::signal(SIGPIPE, SIG_IGN);
#endif

	g_listen_socket = CreateListenSocket(bind_address, port);
	if (g_listen_socket == kInvalidSocket)
	{
		std::cerr << "Unable to listen on " << bind_address << ':' << port << std::endl;
#ifdef _WIN32
		WSACleanup();
#endif
		return 2;
	}
	Log(std::string("Signaling server listening on ") + bind_address + ':' + port +
		" (select, max clients " + std::to_string(kMaxClients) + ')');

	while (!g_stopping)
	{
		fd_set read_set;
		fd_set write_set;
		FD_ZERO(&read_set);
		FD_ZERO(&write_set);
		FD_SET(g_listen_socket, &read_set);
		Socket maximum = g_listen_socket;
		for (const auto& entry : g_clients)
		{
			FD_SET(entry.first, &read_set);
			if (!entry.second->send_buffer.empty()) FD_SET(entry.first, &write_set);
#ifndef _WIN32
			maximum = (std::max)(maximum, entry.first);
#endif
		}
		timeval timeout{};
		timeout.tv_sec = 1;
		const int ready = select(static_cast<int>(maximum + 1), &read_set, &write_set, nullptr, &timeout);
		if (ready < 0)
		{
			if (g_stopping) break;
#ifndef _WIN32
			if (errno == EINTR) continue;
#endif
			Log("select failed");
			break;
		}
		if (ready == 0) continue;
		if (FD_ISSET(g_listen_socket, &read_set)) AcceptClients();

		std::vector<Socket> disconnected;
		for (auto& entry : g_clients)
		{
			if (FD_ISSET(entry.first, &read_set) && !ReadClient(*entry.second))
				disconnected.push_back(entry.first);
			else if (FD_ISSET(entry.first, &write_set) && !WriteClient(*entry.second))
				disconnected.push_back(entry.first);
		}
		for (const Socket socket : disconnected) DisconnectClient(socket);
	}

	std::vector<Socket> sockets;
	for (const auto& entry : g_clients) sockets.push_back(entry.first);
	for (const Socket socket : sockets) DisconnectClient(socket);
	CloseSocket(g_listen_socket);
	g_listen_socket = kInvalidSocket;
#ifdef _WIN32
	WSACleanup();
#endif
	return 0;
}
