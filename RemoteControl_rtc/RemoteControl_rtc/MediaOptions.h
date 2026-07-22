#pragma once

// The requested direction for one remote-control session.
struct MediaOptions
{
	bool send_video = false;
	bool receive_video = true;
	bool send_audio = false;
	bool receive_audio = false;
	bool enable_remote_control = false;
};
