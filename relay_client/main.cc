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
#include "znet/p2p/relay.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/dialer.h"
#include "znet/znet.h"
#include "znet/base/inet_addr.h"

// This part until the locator stuff is directly taken from the basic examples.
enum PacketType {
  PACKET_DEMO
};

class DemoPacket : public znet::Packet {
 public:
  DemoPacket() : Packet(PACKET_DEMO) { }

  std::string text;
};

class DemoSerializer : public znet::PacketSerializer<DemoPacket> {
 public:
  DemoSerializer() : PacketSerializer<DemoPacket>() {}

  std::shared_ptr<znet::Buffer> SerializeTyped(std::shared_ptr<DemoPacket> packet, std::shared_ptr<znet::Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<DemoPacket> DeserializeTyped(std::shared_ptr<znet::Buffer> buffer) override {
    auto packet = std::make_shared<DemoPacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

class MyPacketHandler : public znet::PacketHandler<MyPacketHandler, DemoPacket> {
 public:
  MyPacketHandler(std::shared_ptr<znet::PeerSession> session) : session_(session) { }

  void OnPacket(std::shared_ptr<DemoPacket> p) {
    ZNET_LOG_INFO("Received demo_packet.");
    std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
    pk->text = "Got ya! Hello from server!";
    session_->SendPacket(pk);
  }

 private:
  std::shared_ptr<znet::PeerSession> session_;
};

std::shared_ptr<znet::PeerSession> session_;

// On connect
bool OnConnect(znet::p2p::PeerConnectedEvent& event) {
  session_ = event.session();
  ZNET_LOG_INFO("Connected to peer! punch_id: {}", event.punch_id());

  std::shared_ptr<znet::Codec> codec = std::make_shared<znet::Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  session_->SetCodec(codec);

  session_->SetHandler(std::make_shared<MyPacketHandler>(session_));

  std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
  pk->text = "Hello from client!";
  session_->SendPacket(pk);
  return false;
}

// Locator code
std::unique_ptr<znet::p2p::PeerLocator> locator_;
std::shared_ptr<znet::InetAddress> bind_endpoint_;
std::shared_ptr<znet::InetAddress> target_endpoint_;

bool OnReady(znet::p2p::PeerLocatorReadyEvent& event) {
  ZNET_LOG_INFO("Received peer name from relay: {}", event.peer_name());
  std::string peer_name;
  ZNET_LOG_INFO("Enter peer name:");
  std::cin >> peer_name;
  locator_->AskPeer(peer_name);
  return false;
}

void OnEvent(znet::Event& event) {
  znet::EventDispatcher dispatcher{event};
  dispatcher.Dispatch<znet::p2p::PeerLocatorReadyEvent>(
      ZNET_BIND_GLOBAL_FN(OnReady));
  dispatcher.Dispatch<znet::p2p::PeerConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnect));
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts(
      "relay-client",
      "relay-client is a test utility to test peer to peer connections");
  opts.add_options()
      ("t,target", "Address of the relay server", cxxopts::value<std::string>())
      ("p,port", "Port of the relay server",cxxopts::value<uint16_t>()->default_value("5001"))
          ("h,help", "Print usage");

  auto result = opts.parse(argc, argv);
  if (result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  uint16_t port = result["port"].as<uint16_t>();
  std::string host = result["target"].as<std::string>();

  ZNET_LOG_INFO("Connecting to relay on {}:{}...", host, port);
  znet::p2p::PeerLocatorConfig config{host, port};
  locator_ = std::make_unique<znet::p2p::PeerLocator>(config);
  locator_->SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  znet::Result connect_result;
  if ((connect_result = locator_->Connect()) != znet::Result::Success) {
    ZNET_LOG_ERROR("Failed to connect to relay! Reason: {}", znet::GetResultString(connect_result));
    return 1;  // Failed to connect
  }
  locator_->Wait();
  while (session_ && session_->IsAlive()) {

  }
}