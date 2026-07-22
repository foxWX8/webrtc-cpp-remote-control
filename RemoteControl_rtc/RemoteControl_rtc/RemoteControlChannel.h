#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include "rtc_data_channel.h"

#pragma pack(push, 1)
struct ControlPacket
{
	unsigned int magic = 0x31435452; // RTC1
	unsigned char version = 1;
	unsigned char type = 0;
	unsigned short arg1 = 0;
	int arg2 = 0;
};
#pragma pack(pop)

class RemoteControlChannel final : private libwebrtc::RTCDataChannelObserver
{
public:
	using EventCallback = std::function<void(const std::string&)>;
	~RemoteControlChannel();
	void Attach(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> channel);
	void SetAuthorized(bool authorized) { authorized_ = authorized; }
	void SetEventCallback(EventCallback callback);
	bool IsOpen() const;
	bool UsesSynchronizedMouse() const { return remote_supports_synchronized_mouse_.load(); }
	void SendPing();
	unsigned int RoundTripTimeMs() const { return round_trip_time_ms_.load(); }
	void SendMouseMove(unsigned short x, unsigned short y);
	void SendMouseButton(unsigned char button, bool down, unsigned short x, unsigned short y);
	void SendMouseWheel(short delta);
	void SendKey(unsigned short virtual_key, bool down);

private:
	void SendPacket(const ControlPacket& packet);
	void ReportEvent(const std::string& text);
	void OnStateChange(libwebrtc::RTCDataChannelState state) override;
	void OnMessage(const char* buffer, int length, bool binary) override;
	std::atomic<bool> authorized_ = false;
	std::atomic<unsigned int> round_trip_time_ms_ = 0;
	std::atomic<bool> remote_supports_synchronized_mouse_ = false;
	std::atomic<unsigned short> last_received_mouse_x_ = 0;
	std::atomic<unsigned short> last_received_mouse_y_ = 0;
	std::mutex event_callback_mutex_;
	EventCallback event_callback_;
	libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> channel_;
};
