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

struct UserData {
  std::shared_ptr<znet::PeerSession> session_;
  std::string peer_name_;
};

std::unordered_map<std::string, std::shared_ptr<UserData>> registry_;

class DefaultPacketHandler : public znet::PacketHandler<DefaultPacketHandler, znet::holepunch::RegisterPeerPacket,
                                                        znet::holepunch::ConnectPeerPacket> {
public:
  DefaultPacketHandler(std::shared_ptr<znet::PeerSession> session) : session_(std::move(session)) { }

  void OnPacket(const znet::holepunch::RegisterPeerPacket& pk) {
    auto data = session_->user_ptr_typed<UserData>();
    data->peer_name_ = pk.peer_name_;
    ZNET_LOG_INFO("{} is identified as {} at {}", session_->session_id(),
                  data->peer_name_, session_->remote_address()->readable());
    registry_[pk.peer_name_] = data;

    auto response = std::make_shared<znet::holepunch::RegisterPeerResponsePacket>();
    response->peer_name_ = data->peer_name_;
    response->endpoint_ = session_->remote_address();
    response->ok_ = true;
    session_->SendPacket(response);
  }

  void OnPacket(const znet::holepunch::ConnectPeerPacket& pk) {
    auto data = session_->user_ptr_typed<UserData>();
    if (data->peer_name_.empty()) {
      ZNET_LOG_INFO("{} tried to connect to peer {} but didn't identify itself!",
                    session_->session_id(), pk.target_);
      return;
    }
    auto it = registry_.find(pk.target_);
    if (it == registry_.end()) {
      ZNET_LOG_INFO("{} asked for {} but it was not available.",
                    data->peer_name_, pk.target_);
      return;
    }
    std::shared_ptr<UserData> other_data = it->second;

    auto response = std::make_shared<znet::holepunch::ConnectPeerResponsePacket>();
    response->target_ = other_data->peer_name_;
    response->endpoint_ = other_data->session_->remote_address();
    session_->SendPacket(response);
  }

  void OnUnknown(std::shared_ptr<znet::Packet> p) {
  }

private:
  std::shared_ptr<znet::PeerSession> session_;
};

bool OnConnectEvent(znet::ServerClientConnectedEvent& event) {
  znet::PeerSession& session = *event.session();
  session.SetCodec(znet::holepunch::BuildCodec());
  session.SetHandler(std::make_shared<DefaultPacketHandler>(event.session()));
  session.SetUserPointer(std::make_shared<UserData>());
  return false;
}

void OnEvent(znet::Event& event) {
  znet::EventDispatcher dispatcher{event};
  dispatcher.Dispatch<znet::ServerClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts("relay-server","relay-server is a utility for znet that exchanges peer endpoints");
  opts.add_options()
        ("p,port", "Port to listen on",
            cxxopts::value<uint16_t>()->default_value("5001"))
        ("h,help", "Print usage");

  auto result = opts.parse(argc, argv);
  if (result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  uint16_t port = result["port"].as<uint16_t>();
  ZNET_LOG_INFO("Starting relay on port {}...", port);

  znet::ServerConfig config{"127.0.0.1", port};
  znet::Server relay{config};
  relay.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));


  if (relay.Bind() != znet::Result::Success) {
    return 1; // Failed to bind
  }
  if (relay.Listen() != znet::Result::Success) {
    return 1;  // Failed to listen
  }
  relay.Wait();
  return 0;
}