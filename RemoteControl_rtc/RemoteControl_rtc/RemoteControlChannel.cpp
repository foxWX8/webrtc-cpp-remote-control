#include "pch.h"
#include "RemoteControlChannel.h"
#include "InputInjectorWin.h"

namespace
{
	const char* MouseButtonName(unsigned char button)
	{
		switch (button)
		{
		case 0: return "LEFT";
		case 1: return "RIGHT";
		case 2: return "MIDDLE";
		default: return "UNKNOWN";
		}
	}
}

RemoteControlChannel::~RemoteControlChannel()
{
	if (channel_) { channel_->UnregisterObserver(); channel_->Close(); channel_ = nullptr; }
}

void RemoteControlChannel::Attach(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> channel)
{
	if (channel_) channel_->UnregisterObserver();
	round_trip_time_ms_ = 0;
	remote_supports_synchronized_mouse_ = false;
	channel_ = channel;
	if (channel_) channel_->RegisterObserver(this);
}

void RemoteControlChannel::SetEventCallback(EventCallback callback)
{
	std::lock_guard<std::mutex> lock(event_callback_mutex_);
	event_callback_ = std::move(callback);
}

void RemoteControlChannel::ReportEvent(const std::string& text)
{
	EventCallback callback;
	{
		std::lock_guard<std::mutex> lock(event_callback_mutex_);
		callback = event_callback_;
	}
	if (callback) callback(text);
}

void RemoteControlChannel::OnStateChange(libwebrtc::RTCDataChannelState state)
{
	if (state == libwebrtc::RTCDataChannelOpen) ReportEvent("CONTROL channel OPEN");
	else if (state == libwebrtc::RTCDataChannelClosed) ReportEvent("CONTROL channel CLOSED");
}

bool RemoteControlChannel::IsOpen() const { return channel_ && channel_->state() == libwebrtc::RTCDataChannelOpen; }
void RemoteControlChannel::SendPacket(const ControlPacket& packet) { if (IsOpen()) channel_->Send(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet), true); }
void RemoteControlChannel::SendPing()
{
	ControlPacket p;
	p.type = 6;
	p.arg1 = 1; // Capability bit 0: synchronized mouse position + button.
	p.arg2 = static_cast<int>(::GetTickCount());
	SendPacket(p);
}
void RemoteControlChannel::SendMouseMove(unsigned short x, unsigned short y) { ControlPacket p; p.type = 1; p.arg1 = x; p.arg2 = y; SendPacket(p); }
void RemoteControlChannel::SendMouseButton(unsigned char button, bool down, unsigned short x, unsigned short y)
{
	if (remote_supports_synchronized_mouse_)
	{
		ControlPacket p;
		p.type = 5;
		p.arg1 = x;
		p.arg2 = static_cast<int>(static_cast<unsigned int>(y) |
			(static_cast<unsigned int>(button) << 16) |
			(static_cast<unsigned int>(down ? 1 : 0) << 24));
		SendPacket(p);
		return;
	}
	// Keep mouse buttons compatible with clients built before packet type 5 was
	// introduced. The data channel is ordered, so the coordinate update is
	// applied before the legacy button packet on both old and new receivers.
	SendMouseMove(x, y);
	ControlPacket p;
	p.type = 2;
	p.arg1 = button;
	p.arg2 = down ? 1 : 0;
	SendPacket(p);
}
void RemoteControlChannel::SendMouseWheel(short delta) { ControlPacket p; p.type = 3; p.arg2 = delta; SendPacket(p); }
void RemoteControlChannel::SendKey(unsigned short key, bool down) { ControlPacket p; p.type = 4; p.arg1 = key; p.arg2 = down; SendPacket(p); }

void RemoteControlChannel::OnMessage(const char* buffer, int length, bool binary)
{
	if (!binary || length != sizeof(ControlPacket)) return;
	ControlPacket packet{}; memcpy(&packet, buffer, sizeof(packet));
	if (packet.magic != 0x31435452 || packet.version != 1) return;
	if (packet.type == 6)
	{
		remote_supports_synchronized_mouse_ = (packet.arg1 & 1u) != 0;
		ControlPacket reply;
		reply.type = 7;
		reply.arg1 = 1;
		reply.arg2 = packet.arg2;
		SendPacket(reply);
		return;
	}
	if (packet.type == 7)
	{
		remote_supports_synchronized_mouse_ = (packet.arg1 & 1u) != 0;
		const DWORD elapsed = ::GetTickCount() - static_cast<DWORD>(static_cast<unsigned int>(packet.arg2));
		if (elapsed <= 60000) round_trip_time_ms_ = elapsed > 0 ? elapsed : 1;
		return;
	}
	if (!authorized_)
	{
		if (packet.type == 2 || packet.type == 5)
			ReportEvent("RECV mouse button ignored: remote control is not authorized");
		return;
	}
	switch (packet.type)
	{
	case 1:
	{
		const auto y = static_cast<unsigned short>(packet.arg2);
		last_received_mouse_x_ = packet.arg1;
		last_received_mouse_y_ = y;
		InputInjectorWin::MouseMove(packet.arg1, y);
		break;
	}
	case 2:
	{
		const auto button = static_cast<unsigned char>(packet.arg1);
		const bool down = packet.arg2 != 0;
		const bool injected = InputInjectorWin::MouseButton(button, down);
		char text[160]{};
		sprintf_s(text, "RECV %s %s x=%u y=%u (legacy) SendInput=%s", MouseButtonName(button),
			down ? "DOWN" : "UP", last_received_mouse_x_.load(), last_received_mouse_y_.load(),
			injected ? "OK" : "FAILED");
		ReportEvent(text);
		break;
	}
	case 3:
	{
		const auto delta = static_cast<short>(packet.arg2);
		const bool injected = InputInjectorWin::MouseWheel(delta);
		char text[128]{};
		sprintf_s(text, "RECV WHEEL delta=%d SendInput=%s", delta, injected ? "OK" : "FAILED");
		ReportEvent(text);
		break;
	}
	case 4: InputInjectorWin::Key(packet.arg1, packet.arg2 != 0); break;
	case 5:
	{
		const unsigned int packed = static_cast<unsigned int>(packet.arg2);
		const auto y = static_cast<unsigned short>(packed & 0xffffu);
		const auto button = static_cast<unsigned char>((packed >> 16) & 0xffu);
		const bool down = ((packed >> 24) & 1u) != 0;
		last_received_mouse_x_ = packet.arg1;
		last_received_mouse_y_ = y;
		const bool injected = InputInjectorWin::MouseButtonAt(packet.arg1, y, button, down);
		char text[192]{};
		sprintf_s(text, "RECV %s %s x=%u y=%u (sync) SendInput=%s", MouseButtonName(button),
			down ? "DOWN" : "UP", packet.arg1, y, injected ? "OK" : "FAILED");
		ReportEvent(text);
		break;
	}
	default: break;
	}
}
