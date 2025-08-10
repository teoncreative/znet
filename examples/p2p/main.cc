//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <utility>

#include <deque>
#include "cxxopts.h"
#include "znet/znet.h"
#include "znet/base/inet_addr.h"

using namespace znet;

// Basic setup; locator logic starts below.
enum PacketType {
  PACKET_DEMO
};

class DemoPacket : public Packet {
 public:
  DemoPacket() : Packet(PACKET_DEMO) { }

  std::string text;
};

class DemoSerializer : public PacketSerializer<DemoPacket> {
 public:
  DemoSerializer() : PacketSerializer<DemoPacket>() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<DemoPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<DemoPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<DemoPacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

// Echoes a response when a DemoPacket is received.
class MyPacketHandler : public PacketHandler<MyPacketHandler, DemoPacket> {
 public:
  MyPacketHandler(std::shared_ptr<PeerSession> session) : session_(session) { }

  void OnPacket(std::shared_ptr<DemoPacket> p) {
    ZNET_LOG_INFO("Received demo_packet.");
    std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
    pk->text = "Got ya! Hello from server!";
    session_->SendPacket(pk);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

// Keep the session alive so the connection persists after the locator completes.
std::shared_ptr<PeerSession> session_;

// Called when the peer connection is established.
bool OnConnect(p2p::PeerConnectedEvent& event) {
  session_ = event.session();
  ZNET_LOG_INFO("Connected to peer! punch_id: {}", event.punch_id());

  std::shared_ptr<Codec> codec = std::make_shared<Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  session_->SetCodec(codec);

  session_->SetHandler(std::make_shared<MyPacketHandler>(session_));

  std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
  pk->text = "Hello from client!";
  session_->SendPacket(pk);
  return false;
}

// Peer locator stuff
std::unique_ptr<p2p::PeerLocator> locator_;
std::shared_ptr<InetAddress> bind_endpoint_;
std::shared_ptr<InetAddress> target_endpoint_;

bool OnReady(p2p::PeerLocatorReadyEvent& event) {
  // Show your peer name, ask for the other party’s name, then call locator_->AskPeer(name).
  // This usually happens in UI, but for this example, it is taken from the console.
  ZNET_LOG_INFO("Received peer name from relay: {}", event.peer_name());
  std::string peer_name;
  ZNET_LOG_INFO("Enter peer name:");
  std::cin >> peer_name;
  locator_->AskPeer(peer_name);
  return false;
}

bool OnClose(p2p::PeerLocatorCloseEvent& event) {
  // If no session was created, you can retry here.
  // Keep in mind that you have to do all the previous steps.
  // locator->Connect()
  // Wait for PeerLocatorReadyEvent
  // Ask for a peer
  // Wait for PeerConnectedEvent
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<p2p::PeerLocatorReadyEvent>(
      ZNET_BIND_GLOBAL_FN(OnReady));
  dispatcher.Dispatch<p2p::PeerConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnect));
  dispatcher.Dispatch<p2p::PeerLocatorCloseEvent>(
      ZNET_BIND_GLOBAL_FN(OnClose));
}

// Note: This example requires a publicly accessible relay server.
// The relay must run on a host outside both peers' local networks (not on either peer's machine),
// so it can observe each peer’s public IP address and port.
int main(int argc, char* argv[]) {
  // We are getting the relay information from the command line arguments
  // cxxopts can be removed, and options can be replaced with hard-coded values.
  cxxopts::Options opts(
      "relay-client",
      "relay-client is a test utility to test peer to peer connections");
  opts.add_options()
      ("t,target", "Address of the relay server", cxxopts::value<std::string>())
          ("p,port", "Port of the relay server",cxxopts::value<uint16_t>()->default_value("5001"))
              ("h,help", "Print usage");

  auto parse_result = opts.parse(argc, argv);
  if (parse_result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  uint16_t port = parse_result["port"].as<uint16_t>();
  std::string host = parse_result["target"].as<std::string>();

  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  p2p::PeerLocatorConfig config{host, port};
  // PeerLocator is not designed to be reused, before peer session is created
  // it will stop, and regardless of the status of the punching, it will stay stopped
  // This means that locator->Wait() function will return once punching results.

  // To reuse the locator object, you have to call Connect, and handle the events again
  // It will connect to the relay server, get a different peer name and then
  // you can proceed to ask another peer.
  locator_ = std::make_unique<p2p::PeerLocator>(config);
  locator_->SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  ZNET_LOG_INFO("Connecting to relay on {}:{}...", host, port);
  if ((result = locator_->Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to connect to relay! Reason: {}", GetResultString(result));
    return 1;  // Failed to connect
  }
  locator_->Wait();
  // We wait for the session to complete here.
  while (session_ && session_->IsAlive()) {}
  znet::Cleanup();
}