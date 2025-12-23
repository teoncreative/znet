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
#include "znet/p2p/rendezvous.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server_events.h"
#include "znet/init.h"

#include "cxxopts.h"

#include <utility>
#include <deque>
#include <random>

struct UserData {
  std::shared_ptr<znet::PeerSession> session_;
  std::string peer_name_;
  std::string pending_target_;
};

std::unordered_map<std::string, std::shared_ptr<UserData>> registry_;

std::mutex mutex_;
std::condition_variable cv_;
std::deque<std::shared_ptr<znet::PeerSession>> name_await_queue_;
std::deque<std::shared_ptr<znet::PeerSession>> connect_peer_queue_;
std::deque<std::string> clear_queue_;
std::vector<std::string> names_;

class DefaultPacketHandler
    : public znet::PacketHandler<DefaultPacketHandler,
                                 znet::p2p::IdentifyPacket,
                                 znet::p2p::ConnectPeerPacket> {
 public:
  DefaultPacketHandler(std::shared_ptr<znet::PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(const znet::p2p::IdentifyPacket& pk) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto data = session_->user_ptr_typed<UserData>();
    name_await_queue_.push_front(session_);
    cv_.notify_one();
  }

  void OnPacket(const znet::p2p::ConnectPeerPacket& pk) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto data = session_->user_ptr_typed<UserData>();
    if (data->peer_name_.empty()) {
      ZNET_LOG_INFO(
          "{} tried to connect to peer {} but it wasn't given a peer name!",
          session_->id(), pk.target_peer_);
      return;
    }
    data->pending_target_ = pk.target_peer_;
    connect_peer_queue_.push_front(session_);
    cv_.notify_one();
  }

 private:
  std::shared_ptr<znet::PeerSession> session_;
};

bool OnConnectEvent(znet::IncomingClientConnectedEvent& event) {
  znet::PeerSession& session = *event.session();
  session.SetCodec(znet::p2p::BuildCodec());
  session.SetHandler(std::make_shared<DefaultPacketHandler>(event.session()));
  auto ptr = std::make_shared<UserData>();
  ptr->session_ = event.session();
  session.SetUserPointer(ptr);
  return false;
}

bool OnDisconnectEvent(znet::ServerClientDisconnectedEvent& event) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto data = event.session()->user_ptr_typed<UserData>();
  if (data && !data->peer_name_.empty()) {
    clear_queue_.push_front(data->peer_name_);
    cv_.notify_one();
  }
  return false;
}

void OnEvent(znet::Event& event) {
  znet::EventDispatcher dispatcher{event};
  dispatcher.Dispatch<znet::IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
}

std::string GeneratePeerName() {
  std::string name = znet::GeneratePeerName();
  size_t iterations = 0;
  while (registry_.find(name) != registry_.end()) {
    name = znet::GeneratePeerName();
    iterations++;
    if (iterations > 5000) {
      return "";
    }
  }
  return name;
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts(
      "relay-server",
      "relay-server is a utility for znet that exchanges peer endpoints");
  opts.add_options()
      ("p,port", "Port to listen on",
                     cxxopts::value<uint16_t>()->default_value("5001"))
          ("t,target", "Host to listen on",
           cxxopts::value<std::string>()->default_value("0.0.0.0"))
              ("h,help", "Print usage");

  auto result = opts.parse(argc, argv);
  if (result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  uint16_t port = result["port"].as<uint16_t>();
  std::string target = result["target"].as<std::string>();
  ZNET_LOG_INFO("Starting relay on {}:{}...", target, port);

  znet::ServerConfig config{target, port,std::chrono::seconds(5), znet::ConnectionType::TCP};
  znet::Server relay{config};
  relay.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if (relay.Bind() != znet::Result::Success) {
    return 1;  // Failed to bind
  }
  if (relay.Listen() != znet::Result::Success) {
    return 1;  // Failed to listen
  }

  while (relay.IsAlive()) {
    std::deque<std::shared_ptr<znet::PeerSession>> local_name_q;
    std::deque<std::shared_ptr<znet::PeerSession>> local_connect_q;
    std::deque<std::string> local_clear_q;

    {
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait(lk, [&]() {
        return !name_await_queue_.empty()
               || !connect_peer_queue_.empty()
               || !clear_queue_.empty();
      });

      // O(1) swap: takes ownership of the data, leaves the originals empty
      name_await_queue_.swap(local_name_q);
      connect_peer_queue_.swap(local_connect_q);
      clear_queue_.swap(local_clear_q);
    }
    for (const std::string& peer_name : local_clear_q) {
      registry_.erase(peer_name);
    }
    for (std::shared_ptr<znet::PeerSession>& session : local_name_q) {
      auto data = session->user_ptr_typed<UserData>();
      data->peer_name_ = GeneratePeerName();
      if (data->peer_name_.empty()) {
        ZNET_LOG_ERROR("Failed to select a peer name for {}, disconnecting!",
                       session->id());
        session->Close();
        continue;
      }
      ZNET_LOG_INFO("{} is identified as {} at {}", session->id(),
                    data->peer_name_, session->remote_address()->readable());
      registry_[data->peer_name_] = data;

      auto response = std::make_shared<znet::p2p::SetPeerNamePacket>();
      response->peer_name_ = data->peer_name_;
      response->endpoint_ = session->remote_address();
      session->SendPacket(response);
    }
    for (std::shared_ptr<znet::PeerSession>& session : local_connect_q) {
      auto data = session->user_ptr_typed<UserData>();
      auto it = registry_.find(data->pending_target_);
      if (it == registry_.end()) {
        ZNET_LOG_INFO("{} asked for {} but it was not available yet.",
                      data->peer_name_, data->pending_target_);
        continue;
      }
      std::shared_ptr<UserData> other_data = it->second;
      if (other_data->pending_target_ != data->peer_name_) {
        ZNET_LOG_INFO("{} asked for {}, waiting for other peer to do the same.",
                      data->peer_name_, data->pending_target_);
        continue;
      }
      std::random_device rd;
      std::mt19937 gen(rd());
      uint64_t punch_id = gen();

      auto response = std::make_shared<znet::p2p::StartPunchRequestPacket>();
      // add start_at
      // allow lan connection if possible
      response->target_peer_ = other_data->peer_name_;
      auto other_address = other_data->session_->remote_address();
      auto local_address = session->remote_address();
      response->target_endpoint_ = other_address;
      response->bind_endpoint_ = znet::InetAddress::from(znet::GetAnyBindAddress(local_address->ipv()), local_address->port());
      response->punch_id_ = punch_id;
      response->connection_type_ = znet::ConnectionType::TCP;
      session->SendPacket(response);

      response = std::make_shared<znet::p2p::StartPunchRequestPacket>();
      response->target_peer_ = data->peer_name_;
      response->target_endpoint_ = local_address;
      response->bind_endpoint_ = znet::InetAddress::from(znet::GetAnyBindAddress(other_address->ipv()), other_address->port());
      response->punch_id_ = punch_id;
      response->connection_type_ = znet::ConnectionType::TCP;
      other_data->session_->SendPacket(response);
    }
  }
  relay.Wait();
  return 0;
}