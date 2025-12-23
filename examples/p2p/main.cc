//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/init.h"

#include <utility>
#include <deque>
#include "cxxopts.h"

using namespace znet;

static uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()
              ).count()
  );
}

// Basic setup; locator logic starts below.
enum PacketType {
  PACKET_PING,
  PACKET_PONG,
};

class PingPacket : public Packet {
 public:
  PingPacket() : Packet(PACKET_PING) { }

  uint64_t time;
};

class PongPacket : public Packet {
 public:
  PongPacket() : Packet(PACKET_PONG) { }

  uint64_t time;
};

class PingSerializer : public PacketSerializer<PingPacket> {
 public:
  PingSerializer() : PacketSerializer<PingPacket>() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<PingPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint64_t>(packet->time);
    return buffer;
  }

  std::shared_ptr<PingPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<PingPacket>();
    packet->time = buffer->ReadInt<uint64_t>();
    return packet;
  }
};

class PongSerializer : public PacketSerializer<PongPacket> {
 public:
  PongSerializer() : PacketSerializer<PongPacket>() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<PongPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint64_t>(packet->time);
    return buffer;
  }

  std::shared_ptr<PongPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<PongPacket>();
    packet->time = buffer->ReadInt<uint64_t>();
    return packet;
  }
};

static void SendPing(std::shared_ptr<PeerSession> session) {
  std::shared_ptr<PingPacket> pk = std::make_shared<PingPacket>();
  pk->time = NowMicros();
  session->SendPacket(pk);
}

class MyPacketHandler : public PacketHandler<MyPacketHandler, PingPacket, PongPacket> {
 public:
  MyPacketHandler(std::shared_ptr<PeerSession> session) : session_(session) { }

  void OnPacket(std::shared_ptr<PingPacket> p) {
    std::shared_ptr<PongPacket> reply = std::make_shared<PongPacket>();
    reply->time = p->time;
    session_->SendPacket(reply);

    SendPing(session_);
  }

  void OnPacket(std::shared_ptr<PongPacket> p) {
    uint64_t now_us = NowMicros();
    int64_t rtt_us = static_cast<int64_t>(now_us) - static_cast<int64_t>(p->time);

    if (rtt_us < 0) {
      ZNET_LOG_INFO("Ping: invalid (clock issue)");
      return;
    }

    ZNET_LOG_INFO("Ping: {:.2f} ms", static_cast<double>(rtt_us) / 1000.0);
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
  codec->Add(PACKET_PING, std::make_unique<PingSerializer>());
  codec->Add(PACKET_PONG, std::make_unique<PongSerializer>());
  session_->SetCodec(codec);

  session_->SetHandler(std::make_shared<MyPacketHandler>(session_));

  bool is_server = p2p::IsInitiator(event.punch_id(), event.self_peer_name(),
                                    event.target_peer_name());

  if (is_server) {
    SendPing(session_);
  }
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

// Note: This example requires a publicly accessible rendezvous server.
// The server must run on a host outside both peers' local networks (not on either peer's machine),
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