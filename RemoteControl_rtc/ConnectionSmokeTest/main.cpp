#include "libwebrtc.h"
#include "rtc_desktop_capturer.h"
#include "rtc_desktop_media_list.h"
#include "rtc_peerconnection.h"

#include <condition_variable>
#include <iostream>
#include <mutex>

class Observer final : public libwebrtc::RTCPeerConnectionObserver
{
public:
 void OnSignalingState(libwebrtc::RTCSignalingState) override {}
 void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState) override {}
 void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
 void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
 void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate>) override {}
 void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
 void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
 void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
 void OnRenegotiationNeeded() override {}
 void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver>) override {}
 void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>, libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
 void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
};

int main()
{
 std::cout << "1 Initialize" << std::endl;
 if (!libwebrtc::LibWebRTC::Initialize()) return 1;
 std::cout << "2 Create factory" << std::endl;
 auto factory = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
 if (!factory || !factory->Initialize()) return 2;
 std::cout << "3 Create PeerConnection" << std::endl;
 libwebrtc::RTCConfiguration configuration;
 auto peer = factory->Create(configuration, libwebrtc::RTCMediaConstraints::Create());
 if (!peer) return 3;
 std::cout << "4 Register observer" << std::endl;
 Observer observer;
 peer->RegisterRTCPeerConnectionObserver(&observer);
 std::cout << "5 Create DataChannel" << std::endl;
 libwebrtc::RTCDataChannelInit init;
 auto channel = peer->CreateDataChannel("remote-control", &init);
 std::cout << "6 Get desktop device" << std::endl;
 auto device = factory->GetDesktopDevice();
 if (!device) return 4;
 std::cout << "7 Update screen list" << std::endl;
 auto list = device->GetDesktopMediaList(libwebrtc::kScreen);
 const int update_result = list ? list->UpdateSourceList(true, false) : -999;
 std::cout << "update_result=" << update_result << " count=" << (list ? list->GetSourceCount() : -1) << std::endl;
 if (!list || list->GetSourceCount() <= 0) return 5;
 std::cout << "8 Create desktop capturer" << std::endl;
 auto capturer = device->CreateDesktopCapturer(list->GetSource(0), true);
 std::cout << "9 Create desktop source" << std::endl;
 auto source = factory->CreateDesktopSource(capturer, "desktop-source", libwebrtc::RTCMediaConstraints::Create());
 std::cout << "10 Create video track" << std::endl;
 auto track = factory->CreateVideoTrack(source, "desktop-video");
 std::cout << "11 Start capturer" << std::endl;
 const auto state = capturer->Start(30);
 std::cout << "start_state=" << static_cast<int>(state) << std::endl;
 std::cout << "12 Add video track" << std::endl;
 std::vector<libwebrtc::string> stream_ids{libwebrtc::string("desktop-stream")};
 if (!peer->AddTrack(track, libwebrtc::vector<libwebrtc::string>(stream_ids))) return 6;
 std::cout << "13 Create offer" << std::endl;
 std::mutex offer_mutex;
 std::condition_variable offer_ready;
 bool offer_finished = false;
 bool offer_succeeded = false;
 peer->CreateOffer(
  [&](const libwebrtc::string& sdp, const libwebrtc::string&) {
   std::lock_guard<std::mutex> lock(offer_mutex);
   offer_succeeded = !sdp.std_string().empty(); offer_finished = true; offer_ready.notify_one();
  },
  [&](const char*) {
   std::lock_guard<std::mutex> lock(offer_mutex);
   offer_finished = true; offer_ready.notify_one();
  }, libwebrtc::RTCMediaConstraints::Create());
 {
  std::unique_lock<std::mutex> lock(offer_mutex);
  offer_ready.wait_for(lock, std::chrono::seconds(5), [&] { return offer_finished; });
 }
 std::cout << "offer_finished=" << offer_finished << " offer_succeeded=" << offer_succeeded << std::endl;
 if (!offer_succeeded) return 7;
 std::cout << "14 Cleanup" << std::endl;
 if (capturer->IsRunning()) capturer->Stop();
 track = nullptr;
 source = nullptr;
 capturer = nullptr;
 list = nullptr;
 device = nullptr;
 channel = nullptr;
 peer->DeRegisterRTCPeerConnectionObserver();
 peer->Close();
 peer = nullptr;
 factory->Terminate();
 factory = nullptr;
 libwebrtc::LibWebRTC::Terminate();
 std::cout << "PASS" << std::endl;
 return 0;
}
