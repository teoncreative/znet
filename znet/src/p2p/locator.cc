//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 08/08/2025.
//

#include "znet/p2p/locator.h"
#include "znet/p2p/relay.h"

namespace znet {
namespace p2p {

class LocatorPacketHandler : public PacketHandler<LocatorPacketHandler, SetPeerNamePacket, StartPunchRequestPacket> {
 public:
  LocatorPacketHandler(PeerLocator& locator) : locator_(locator) { }

  void OnPacket(const SetPeerNamePacket& pk) {
    locator_.SetPeerName(pk.peer_name_, pk.endpoint_);
  }

  void OnPacket(const StartPunchRequestPacket& pk) {
    StartPunchRequestEvent event{pk.target_peer_, pk.bind_port_, pk.target_endpoint_};
    locator_.event_callback_(event);
  }

 private:
  PeerLocator& locator_;
};


PeerLocator::PeerLocator(const znet::p2p::PeerLocatorConfig& config)
    : client_(ClientConfig{config.server_ip, config.server_port, std::chrono::seconds(10),
      ConnectionType::TCP}) {
  client_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

PeerLocator::~PeerLocator() {

}

Result PeerLocator::Start() {
  Result result;
  if ((result = client_.Bind()) != Result::Success) {
    return result;
  }
  if ((result = client_.Connect()) != Result::Success) {
    return result;
  }
  return result;
}

void PeerLocator::AskPeer(std::string peer_name) {
  auto pk = std::make_shared<ConnectPeerPacket>();
  pk->target_peer_ = peer_name;
  session_->SendPacket(pk);
}

void PeerLocator::Wait() {
  client_.Wait();
}

void PeerLocator::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(ZNET_BIND_FN(OnConnectEvent));
}

bool PeerLocator::OnConnectEvent(ClientConnectedToServerEvent& event) {
  session_ = event.session();
  session_->SetCodec(BuildCodec());
  session_->SetHandler(std::make_shared<LocatorPacketHandler>(*this));
  auto pk = std::make_shared<IdentifyPacket>();
  pk->port_ = 50200;
  session_->SendPacket(pk);
  return false;
}

void PeerLocator::SetPeerName(std::string peer_name, std::shared_ptr<InetAddress> endpoint) {
  peer_name_ = peer_name;
  endpoint_ = endpoint;
  PeerLocatorReadyEvent event{peer_name, endpoint_};
  event_callback_(event);
}

}
}