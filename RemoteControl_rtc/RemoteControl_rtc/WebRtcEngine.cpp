#include "pch.h"
#include "WebRtcEngine.h"

WebRtcEngine::~WebRtcEngine()
{
	Shutdown();
}

bool WebRtcEngine::Initialize()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_)
	{
		return true;
	}

	if (!libwebrtc::LibWebRTC::Initialize())
	{
		return false;
	}

	factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
	if (!factory_ || !factory_->Initialize())
	{
		factory_ = nullptr;
		libwebrtc::LibWebRTC::Terminate();
		return false;
	}

	initialized_ = true;
	return true;
}

void WebRtcEngine::Shutdown()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_)
	{
		return;
	}

	if (factory_)
	{
		factory_->Terminate();
		factory_ = nullptr;
	}
	libwebrtc::LibWebRTC::Terminate();
	initialized_ = false;
}

bool WebRtcEngine::IsInitialized() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return initialized_;
}

libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> WebRtcEngine::Factory() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return factory_;
}
