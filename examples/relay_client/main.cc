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

#include "znet/znet.h"
#include "znet/p2p/relay.h"
#include "cxxopts.h"

const std::string kPeerName = GeneratePeerName();

class DefaultPacketHandler : public znet::PacketHandler<DefaultPacketHandler,
                                                        znet::holepunch::RegisterPeerResponsePacket,
                                                        znet::holepunch::ConnectPeerResponsePacket> {
public:
  DefaultPacketHandler(std::shared_ptr<znet::PeerSession> session) : session_(std::move(session)) { }

  void OnPacket(const znet::holepunch::RegisterPeerResponsePacket& pk) {
    ZNET_LOG_INFO("RegisterPeerResponsePacket {}, ok: {}", pk.peer_name_, pk.ok_);
  }

  void OnPacket(const znet::holepunch::ConnectPeerResponsePacket& pk) {
    ZNET_LOG_INFO("ConnectPeerResponsePacket target: {}, endpoint: {}", pk.target_, pk.endpoint_->readable());
  }

  void OnUnknown(std::shared_ptr<znet::Packet> p) {
  }

private:
  std::shared_ptr<znet::PeerSession> session_;
};

bool OnConnectEvent(znet::ClientConnectedToServerEvent& event) {
  znet::PeerSession& session = *event.session();
  session.SetCodec(znet::holepunch::BuildCodec());
  session.SetHandler(std::make_shared<DefaultPacketHandler>(event.session()));

  auto pk = std::make_shared<znet::holepunch::RegisterPeerPacket>();
  pk->peer_name_ = kPeerName;
  pk->port_ = 6000; // Actual game endpoint port
  session.SendPacket(pk);
  return false;
}

void OnEvent(znet::Event& event) {
  znet::EventDispatcher dispatcher{event};
  dispatcher.Dispatch<znet::ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts("punch-client","znet hole‑punch client");
  opts.add_options()
      ("r,relay",  "relay IP",   cxxopts::value<std::string>()->default_value("127.0.0.1"))
          ("p,port",   "relay port", cxxopts::value<uint16_t>()->default_value("5001"))
              ("t,target", "peer name",  cxxopts::value<std::string>())
                  ("l,local",  "punch port", cxxopts::value<uint16_t>()->default_value("6000"))
                      ("h,help",   "help");
  auto res = opts.parse(argc, argv);
  if (res.count("help") || !res.count("name") || !res.count("target")) {
    std::cout<<opts.help();
    return 0;
  }

  std::string relay_ip   = res["relay"].as<std::string>();
  uint16_t    relay_port = res["port" ].as<uint16_t>();
  std::string peer        = res["target"].as<std::string>();
  uint16_t    local_port  = res["local"].as<uint16_t>();

  ZNET_LOG_INFO("I am the peer {}", kPeerName);
  znet::ClientConfig config{relay_ip, relay_port};
  znet::Client client{config};
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if (client.Bind() != znet::Result::Success) {
    return 1; // Failed to bind
  }
  if (client.Connect() != znet::Result::Success) {
    return 1; // Failed to connect
  }
  client.Wait();
  return 0;
}