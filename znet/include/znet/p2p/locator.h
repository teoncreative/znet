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

#ifndef ZNET_PARENT_LOCATOR_H
#define ZNET_PARENT_LOCATOR_H

#include "znet/client.h"
#include "znet/precompiled.h"
#include "znet/base/event.h"
#include "znet/client_events.h"

namespace znet {
namespace p2p {

struct PeerLocatorConfig {
  std::string server_ip;
  PortNumber server_port;
};

class PeerLocatorReadyEvent : public Event {
 public:
  explicit PeerLocatorReadyEvent(std::string peer_name, std::shared_ptr<InetAddress> endpoint)
      : peer_name_(peer_name), endpoint_(endpoint) {}

  const std::string& peer_name() const { return peer_name_; }
  std::shared_ptr<InetAddress> endpoint() const { return endpoint_; }

  ZNET_EVENT_CLASS_TYPE(PeerLocatorReadyEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};

class StartPunchRequestEvent : public Event {
 public:
  explicit StartPunchRequestEvent(std::string target_peer, PortNumber bind_port, std::shared_ptr<InetAddress> target_endpoint)
      : target_peer_(target_peer), bind_port_(bind_port), target_endpoint_(target_endpoint) {}

  const std::string& target_peer() const { return target_peer_; }
  PortNumber bind_port() const { return bind_port_; }
  std::shared_ptr<InetAddress> target_endpoint() const { return target_endpoint_; }

  ZNET_EVENT_CLASS_TYPE(StartPunchRequestEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::string target_peer_;
  PortNumber bind_port_;
  std::shared_ptr<InetAddress> target_endpoint_;
};

class PeerLocator {
 public:
  PeerLocator(const PeerLocatorConfig&);
  PeerLocator(const PeerLocator&) = delete;
  ~PeerLocator();

  Result Start();

  void Wait();

  void AskPeer(std::string peer_name);

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD EventCallbackFn event_callback() const {
    return event_callback_;
  }

  const std::string& peer_name() const { return peer_name_; }

 private:
  friend class LocatorPacketHandler;

  void OnEvent(Event&);
  bool OnConnectEvent(ClientConnectedToServerEvent& event);

  void SetPeerName(std::string peer_name, std::shared_ptr<InetAddress> endpoint);

  EventCallbackFn event_callback_;
  Client client_;
  std::shared_ptr<PeerSession> session_;
  PortNumber game_port_;

  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};


}
}

#endif  //ZNET_PARENT_LOCATOR_H
