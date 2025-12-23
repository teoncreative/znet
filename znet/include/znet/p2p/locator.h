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

#include "znet/event.h"
#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/precompiled.h"

namespace znet {
namespace p2p {

struct PeerLocatorConfig {
  std::string server_ip;
  PortNumber server_port;
  ConnectionType connection_type = ConnectionType::TCP;
};

class PeerLocatorReadyEvent : public Event {
 public:
  explicit PeerLocatorReadyEvent(std::string peer_name,
                                 std::shared_ptr<InetAddress> endpoint)
      : peer_name_(peer_name), endpoint_(endpoint) {}

  const std::string& peer_name() const { return peer_name_; }

  std::shared_ptr<InetAddress> endpoint() const { return endpoint_; }

  ZNET_EVENT_CLASS_TYPE(PeerLocatorReadyEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};

class PeerLocatorCloseEvent : public Event {
 public:
  explicit PeerLocatorCloseEvent() {}

  ZNET_EVENT_CLASS_TYPE(PeerLocatorCloseEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
};

class PeerConnectedEvent : public Event {
 public:
  explicit PeerConnectedEvent(std::shared_ptr<PeerSession> session,
                              uint64_t punch_id, std::string self_peer_name,
                              std::string target_peer_name)
      : session_(session),
        punch_id_(punch_id),
        self_peer_name_(self_peer_name),
        target_peer_name_(target_peer_name) {}

  std::shared_ptr<PeerSession> session() const { return session_; }

  uint64_t punch_id() const { return punch_id_; }

  const std::string& self_peer_name() const { return self_peer_name_; }

  const std::string& target_peer_name() const { return target_peer_name_; }

  ZNET_EVENT_CLASS_TYPE(PeerConnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryP2P)
 private:
  std::shared_ptr<PeerSession> session_;
  uint64_t punch_id_;
  std::string self_peer_name_;
  std::string target_peer_name_;
};

class PeerLocator {
 public:
  PeerLocator(const PeerLocatorConfig&);
  PeerLocator(const PeerLocator&) = delete;
  ~PeerLocator();

  Result Connect();
  Result Disconnect();

  void Wait();

  Result AskPeer(std::string peer_name);

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD EventCallbackFn event_callback() const {
    return event_callback_;
  }

  const std::string& peer_name() const { return peer_name_; }

 private:
  friend class LocatorPacketHandler;

  void OnEvent(Event&);
  bool OnConnectEvent(ClientConnectedToServerEvent& event);
  bool OnDisconnectEvent(ClientDisconnectedFromServerEvent& event);

  void SetPeerName(std::string peer_name,
                   std::shared_ptr<InetAddress> endpoint);

  EventCallbackFn event_callback_;
  Client client_;
  PortNumber game_port_;

  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
  std::shared_ptr<PeerSession> session_;

  std::mutex mutex_;
  std::condition_variable cv_;
  Task task_;

  std::shared_ptr<InetAddress> bind_endpoint_;
  std::shared_ptr<InetAddress> target_endpoint_;
  ConnectionType connection_type_;
  std::string target_peer_name_;
  uint64_t punch_id_ = ~0;

  bool is_running_ = false;
};

}  // namespace p2p
}  // namespace znet

#endif  //ZNET_PARENT_LOCATOR_H
