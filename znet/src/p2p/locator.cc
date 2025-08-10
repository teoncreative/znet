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
#include "znet/p2p/dialer.h"
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
    ZNET_LOG_INFO("Received punch request to {}, {} (local) -> {} (remote)", pk.target_peer_, pk.bind_endpoint_->readable(),
                  pk.target_endpoint_->readable());
    locator_.target_endpoint_ = pk.target_endpoint_;
    locator_.bind_endpoint_ = pk.bind_endpoint_;
    locator_.punch_id_ = pk.punch_id_;
    locator_.target_peer_name_ = pk.target_peer_;
    CloseOptions options;
    options.Set<NoLingerKey>(true);
    locator_.client_.Disconnect(options);
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
  Disconnect();
}

Result PeerLocator::Connect() {
  if (is_running_) {
    return Result::AlreadyConnected;
  }
  is_running_ = true;
  peer_name_ = "";
  session_ = nullptr;

  bind_endpoint_ = nullptr;
  target_endpoint_ = nullptr;
  punch_id_ = ~0;

  task_.Run([this]() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock);
    is_running_ = false;
    if (bind_endpoint_ && target_endpoint_) {
      Result result;
      std::shared_ptr<PeerSession> session =
          PunchSync(
              bind_endpoint_,
              target_endpoint_,
              &result,
              IsInitiator(punch_id_, peer_name_, target_peer_name_)
          );
      if (result == Result::Success) {
        PeerConnectedEvent event{session, punch_id_, peer_name_, target_peer_name_};
        event_callback_(event);
        return;
      }
    }
    PeerLocatorCloseEvent event;
    event_callback_(event);
  });

  Result result;
  if ((result = client_.Bind()) != Result::Success) {
    return result;
  }
  if ((result = client_.Connect()) != Result::Success) {
    return result;
  }
  ZNET_LOG_INFO("Relay client bound to {} and connected to {}", client_.local_address()->readable(),
                client_.server_address()->readable());
  return result;
}

Result PeerLocator::Disconnect() {
  return client_.Disconnect();
}

Result PeerLocator::AskPeer(std::string peer_name) {
  if (!session_ || !session_->IsAlive()) {
    return Result::NotConnected;
  }
  auto pk = std::make_shared<ConnectPeerPacket>();
  pk->target_peer_ = peer_name;
  session_->SendPacket(pk);
  return Result::Success;
}

void PeerLocator::Wait() {
  client_.Wait();
  task_.Wait();
}

void PeerLocator::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(ZNET_BIND_FN(OnConnectEvent));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(ZNET_BIND_FN(OnDisconnectEvent));
}

bool PeerLocator::OnConnectEvent(ClientConnectedToServerEvent& event) {
  session_ = event.session();
  session_->SetCodec(BuildCodec());
  session_->SetHandler(std::make_shared<LocatorPacketHandler>(*this));
  session_->SendPacket(std::make_shared<IdentifyPacket>());
  return false;
}

bool PeerLocator::OnDisconnectEvent(ClientDisconnectedFromServerEvent& event) {
  cv_.notify_all();
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