//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/rendezvous_server.h"

#include "znet/util.h"

namespace znet {
namespace p2p {

class RendezvousPacketHandler
    : public PacketHandler<RendezvousPacketHandler, IdentifyPacket,
                           ConnectPeerPacket> {
 public:
  RendezvousPacketHandler(RendezvousServer& server,
                          std::shared_ptr<PeerSession> session)
      : server_(server), session_(std::move(session)) {}

  void OnPacket(const IdentifyPacket& pk) {
    {
      std::lock_guard<std::mutex> lock(server_.mutex_);
      auto data = session_->user_pointer<RendezvousServer::ClientData>();
      if (!server_.AllowRequest(*data)) {
        return;
      }
      // the claim is only ever relayed to this client's match, so the worst
      // a lie can do is cost that match one wasted punch candidate
      if (pk.local_endpoint_ && pk.local_endpoint_->is_valid() &&
          pk.local_endpoint_->ipv() != InetProtocolVersion::Unix) {
        data->private_endpoint = pk.local_endpoint_;
      }
      data->punch_port = pk.punch_port_;
      server_.name_await_queue_.push_front(session_);
    }
    server_.cv_.notify_one();
  }

  void OnPacket(const ConnectPeerPacket& pk) {
    {
      std::lock_guard<std::mutex> lock(server_.mutex_);
      auto data = session_->user_pointer<RendezvousServer::ClientData>();
      if (!server_.AllowRequest(*data)) {
        return;
      }
      if (data->peer_name.empty()) {
        ZNET_LOG_INFO(
            "{} tried to connect to peer {} but it wasn't given a peer name!",
            session_->id(), pk.target_peer_);
        return;
      }
      data->pending_targets.insert(pk.target_peer_);
      server_.connect_peer_queue_.push_front(
          std::make_pair(session_, pk.target_peer_));
    }
    server_.cv_.notify_one();
  }

 private:
  RendezvousServer& server_;
  std::shared_ptr<PeerSession> session_;
};

namespace {

ServerConfig MakeRelayConfig(const RendezvousServer::Config& config) {
  ServerConfig out{};
  out.bind_address = config.bind_address;
  out.bind_port = config.bind_port;
  out.connection_timeout = std::chrono::seconds(5);
  out.connection_type = ConnectionType::TCP;
  // the relay is a Server like any other, so the listener-level protections
  // (lists, per-source connection throttle, max_connections) come from here
  out.options = config.options;
  return out;
}

}  // namespace

RendezvousServer::RendezvousServer(const Config& config)
    : config_(config),
      server_(MakeRelayConfig(config)),
      punch_id_rng_(std::random_device{}()) {
  // before Listen(): MainProcessor fires events unconditionally
  server_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

RendezvousServer::~RendezvousServer() {
  Stop();
  Wait();
}

Result RendezvousServer::Start() {
  Result result = server_.Bind();
  if (result != Result::Success) {
    return result;
  }
  result = server_.Listen();
  if (result != Result::Success) {
    return result;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = false;
  }
  pairing_task_.Run([this]() { PairingLoop(); });
  return Result::Success;
}

void RendezvousServer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  server_.Stop();
}

void RendezvousServer::Wait() {
  pairing_task_.Wait();
  server_.Wait();
}

void RendezvousServer::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_FN(OnConnectEvent));
  dispatcher.Dispatch<IncomingClientDisconnectedEvent>(
      ZNET_BIND_FN(OnDisconnectEvent));
}

bool RendezvousServer::OnConnectEvent(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();
  session.SetCodec(BuildRendezvousCodec());
  session.SetHandler(
      std::make_shared<RendezvousPacketHandler>(*this, event.session()));
  auto data = std::make_shared<ClientData>();
  data->session = event.session();
  session.SetUserPointer(data);
  return false;
}

bool RendezvousServer::OnDisconnectEvent(IncomingClientDisconnectedEvent& event) {
  auto data = event.session()->user_pointer<ClientData>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data && !data->peer_name.empty()) {
      clear_queue_.push_front(data->peer_name);
    } else {
      return false;
    }
  }
  cv_.notify_one();
  return false;
}

void RendezvousServer::PairingLoop() {
  for (;;) {
    std::deque<std::shared_ptr<PeerSession>> local_name_q;
    std::deque<std::pair<std::shared_ptr<PeerSession>, std::string>>
        local_connect_q;
    std::deque<std::string> local_clear_q;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() {
        return stop_ || !name_await_queue_.empty() ||
               !connect_peer_queue_.empty() || !clear_queue_.empty();
      });
      if (stop_) {
        return;
      }
      // O(1) swap: takes ownership of the data, leaves the originals empty
      name_await_queue_.swap(local_name_q);
      connect_peer_queue_.swap(local_connect_q);
      clear_queue_.swap(local_clear_q);
    }
    // re-taken per item rather than held across the batch, so the packet
    // handlers on the session workers are never blocked for long. SendPacket
    // only queues, so calling it under the lock costs nothing.
    for (const std::string& peer_name : local_clear_q) {
      std::lock_guard<std::mutex> lock(mutex_);
      registry_.erase(peer_name);
    }
    for (std::shared_ptr<PeerSession>& session : local_name_q) {
      AssignName(session);
    }
    for (auto& ask : local_connect_q) {
      TryPair(ask.first, ask.second);
    }
  }
}

bool RendezvousServer::AllowRequest(ClientData& data) {
  if (config_.max_requests_per_window == 0 ||
      config_.request_window.count() <= 0) {
    return true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (data.request_count == 0 ||
      now - data.request_window_start > config_.request_window) {
    data.request_window_start = now;
    data.request_count = 0;
  }
  data.request_count++;
  if (data.request_count <= config_.max_requests_per_window) {
    return true;
  }
  // over the limit the client is dropped, so spam costs a reconnect and the
  // connection throttle in ServerOptions prices those
  ZNET_LOG_WARN("Disconnecting {}: over {} locator requests in the window.",
                data.session->id(), config_.max_requests_per_window);
  CloseOptions options;
  options.Set<NoLingerKey>(true);
  data.session->Close(options);
  return false;
}

void RendezvousServer::AssignName(const std::shared_ptr<PeerSession>& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto data = session->user_pointer<ClientData>();
  if (!data->peer_name.empty()) {
    // identify is idempotent: a repeat gets the same name again. Minting a
    // fresh one would also leak the old registry entry until disconnect,
    // which made repeated identifies a free way to bloat the registry.
    auto response = std::make_shared<SetPeerNamePacket>();
    response->peer_name_ = data->peer_name;
    response->endpoint_ = session->remote_address();
    session->SendPacket(response);
    return;
  }
  data->peer_name = GenerateUniqueName();
  if (data->peer_name.empty()) {
    ZNET_LOG_ERROR("Failed to select a peer name for {}, disconnecting!",
                   session->id());
    session->Close();
    return;
  }
  ZNET_LOG_INFO("{} is identified as {} at {}", session->id(), data->peer_name,
                session->remote_address()->readable());
  registry_[data->peer_name] = data;

  auto response = std::make_shared<SetPeerNamePacket>();
  response->peer_name_ = data->peer_name;
  response->endpoint_ = session->remote_address();
  session->SendPacket(response);
}

void RendezvousServer::TryPair(const std::shared_ptr<PeerSession>& session,
                               const std::string& target) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto data = session->user_pointer<ClientData>();
  if (data->pending_targets.count(target) == 0) {
    return;  // already paired by the other side's ask in this same batch
  }
  auto it = registry_.find(target);
  if (it == registry_.end()) {
    ZNET_LOG_INFO("{} asked for {} but it was not available yet.",
                  data->peer_name, target);
    data->pending_targets.erase(target);
    auto response = std::make_shared<PeerNotFoundPacket>();
    response->target_peer_ = target;
    session->SendPacket(response);
    return;
  }
  std::shared_ptr<ClientData> other_data = it->second;
  if (other_data->pending_targets.count(data->peer_name) == 0) {
    ZNET_LOG_INFO("{} asked for {}, waiting for other peer to do the same.",
                  data->peer_name, target);
    return;
  }
  // both asks are consumed by the pair; a later re-ask starts fresh
  data->pending_targets.erase(target);
  other_data->pending_targets.erase(data->peer_name);
  const uint64_t punch_id = punch_id_rng_();

  // the observed address carries the truth about the IP; the port a client
  // punches from is its own claim (its relay port, or a Host's UDP port)
  auto punch_endpoint_of = [](const std::shared_ptr<ClientData>& client) {
    std::shared_ptr<InetAddress> observed = client->session->remote_address();
    if (client->punch_port != 0 && client->punch_port != observed->port()) {
      return std::shared_ptr<InetAddress>(
          observed->WithPort(client->punch_port));
    }
    return observed;
  };
  auto other_address = punch_endpoint_of(other_data);
  auto local_address = punch_endpoint_of(data);

  // a private address identical to the observed one carries no information;
  // only a distinct claim is worth a punch candidate
  auto private_of = [](const std::shared_ptr<ClientData>& client,
                       const std::shared_ptr<InetAddress>& observed) {
    if (client->private_endpoint &&
        client->private_endpoint->readable() != observed->readable()) {
      return client->private_endpoint;
    }
    return std::shared_ptr<InetAddress>();
  };

  auto response = std::make_shared<StartPunchRequestPacket>();
  response->target_peer_ = other_data->peer_name;
  response->target_endpoint_ = other_address;
  response->target_private_endpoint_ = private_of(other_data, other_address);
  response->punch_id_ = punch_id;
  response->connection_type_ = config_.punch_connection_type;
  session->SendPacket(response);

  response = std::make_shared<StartPunchRequestPacket>();
  response->target_peer_ = data->peer_name;
  response->target_endpoint_ = local_address;
  response->target_private_endpoint_ = private_of(data, local_address);
  response->punch_id_ = punch_id;
  response->connection_type_ = config_.punch_connection_type;
  other_data->session->SendPacket(response);
}

std::string RendezvousServer::GenerateUniqueName() {
  // caller holds mutex_
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

}  // namespace p2p
}  // namespace znet
