#pragma once

#include <mutex>

#include "libwebrtc.h"

// Owns the process-wide libwebrtc runtime and its PeerConnectionFactory.
// All PeerSession instances must be destroyed before Shutdown().
class WebRtcEngine final
{
public:
	WebRtcEngine() = default;
	~WebRtcEngine();

	WebRtcEngine(const WebRtcEngine&) = delete;
	WebRtcEngine& operator=(const WebRtcEngine&) = delete;

	bool Initialize();
	void Shutdown();
	bool IsInitialized() const;

	libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> Factory() const;

private:
	mutable std::mutex mutex_;
	libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory_;
	bool initialized_ = false;
};
