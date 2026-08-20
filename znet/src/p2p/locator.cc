//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/locator.h"
#include "znet/p2p/dialer.h"
#include "znet/p2p/rendezvous.h"

namespace znet {
namespace p2p {

namespace {

// Every local address at the port punches leave from
std::vector<std::shared_ptr<InetAddress>> LocalPunchEndpoints(PortNumber port) {
  std::vector<std::shared_ptr<InetAddress>> endpoints;
  for (const auto& address : GetLocalAddresses(InetProtocolVersion::IPv4)) {
    auto endpoint = InetAddress::from(address, port);
    if (endpoint && endpoint->is_valid()) {
      endpoints.push_back(std::move(endpoint));
    }
    if (endpoints.size() >= kMaxPrivateEndpoints) {
      break;
    }
  }
  return endpoints;
}

}  // namespace

class LocatorPacketHandler : public PacketHandler<LocatorPacketHandler, SetPeerNamePacket, StartPunchRequestPacket, PeerNotFoundPacket> {
 public:
  LocatorPacketHandler(PeerLocator& locator) : locator_(locator) { }

  void OnPacket(const SetPeerNamePacket& pk) {
    locator_.SetPeerName(pk.peer_name_, pk.endpoint_);
  }

  void OnPacket(const PeerNotFoundPacket& pk) {
    locator_.OnPeerNotFound(pk.target_peer_);
  }

  void OnPacket(const StartPunchRequestPacket& pk) {
    ZNET_LOG_INFO("Received punch request to {} at {}", pk.target_peer_,
                  pk.target_endpoint_->readable());
    {
      // the worker reads these under the same mutex once it wakes
      std::lock_guard<std::mutex> lock(locator_.mutex_);
      locator_.target_endpoint_ = pk.target_endpoint_;
      locator_.target_private_endpoints_ = pk.target_private_endpoints_;
      locator_.punch_id_ = pk.punch_id_;
      locator_.target_peer_name_ = pk.target_peer_;
      locator_.connection_type_ = pk.connection_type_;
    }
    CloseOptions options;
    options.Set<NoLingerKey>(true);
    locator_.client_.Disconnect(options);
  }

 private:
  PeerLocator& locator_;
};

PeerLocator::PeerLocator(const PeerLocatorConfig& config)
    : client_(ClientConfig{config.server_address,         // server_address
                           config.server_port,       // server_port
                           std::chrono::seconds(10), // connection_timeout
                           ConnectionType::TCP,      // connection_type
                           {}}) {                    // options
  client_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

PeerLocator::~PeerLocator() {
  Disconnect();
  // The worker sleeps on cv_; RequestStop alone never wakes it and ~Task
  // would deadlock on the join. The empty lock pairs with the predicate
  // check so the wakeup cannot fall between check and block.
  task_.RequestStop();
  { std::lock_guard<std::mutex> lock(mutex_); }
  cv_.notify_all();
  task_.Wait();
}

Result PeerLocator::Connect() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_running_) {
      return Result::AlreadyConnected;
    }
    is_running_ = true;
    wake_ = false;
  }
  peer_name_ = "";
  session_ = nullptr;

  target_endpoint_ = nullptr;
  punch_id_ = kInvalidPunchId;

  Result result;
  if (ZNET_UNLIKELY((result = client_.Bind()) != Result::Success)) ZNET_UNLIKELY_ATTR {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  if (ZNET_UNLIKELY((result = client_.Connect()) != Result::Success)) ZNET_UNLIKELY_ATTR {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }

  // started only after the connect took: a failed Connect() leaves no worker
  // parked on cv_ and the locator free to try again. A disconnect racing this
  // is not lost, since wake_ is a flag the predicate rechecks, not a signal.
  task_.Run([this]() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return wake_ || task_.IsStopRequested(); });
    is_running_ = false;
    if (task_.IsStopRequested()) {
      lock.unlock();
      PeerLocatorCloseEvent event;
      if (event_callback_) {
        event_callback_(event);
      }
      return;
    }
    // copy what the punch needs and let go of the mutex: the callbacks other
    // threads run take it, and a punch can last seconds
    //
    // bind the exact interface and port the relay connection used: the punch
    // reuses that socket's NAT mapping, and a wildcard bind would collide
    // with any unrelated connection whose TIME_WAIT holds the same port on
    // another interface
    auto bind_endpoint = client_.local_address();
    auto target_endpoint = target_endpoint_;
    auto target_privates = target_private_endpoints_;
    const uint64_t punch_id = punch_id_;
    const std::string self_name = peer_name_;
    const std::string target_name = target_peer_name_;
    const ConnectionType connection_type = connection_type_;
    session_ = nullptr;
    lock.unlock();
    if (bind_endpoint && target_endpoint) {
      // the punch binds the very port the relay connection used, so that
      // socket has to be fully released, not just shut down: its descriptor
      // closes with the last reference to the session
      client_.Wait();
      client_.ReleaseSession();
      // the public endpoint first; every private one the peer reported joins
      // the race, which is what makes two peers on a shared network reachable
      std::vector<std::shared_ptr<InetAddress>> candidates;
      candidates.push_back(target_endpoint);
      for (const auto& target_private : target_privates) {
        if (target_private->readable() != target_endpoint->readable()) {
          candidates.push_back(target_private);
        }
      }
      Result punch_result;
      std::shared_ptr<PeerSession> session =
          PunchSync(bind_endpoint, candidates,
                    IsInitiator(punch_id, self_name, target_name),
                    connection_type, std::chrono::milliseconds(5000),
                    &punch_result);
      if (punch_result == Result::Success) {
        PeerConnectedEvent event{session, punch_id, self_name, target_name};
        if (event_callback_) {
          event_callback_(event);
        }
        return;
      }
      ZNET_LOG_ERROR("Punch failed with reason: {}", GetResultString(punch_result));
      FireFailed(PeerLocatorPhase::Punch, punch_result, target_name);
      // falls through to the close event: no session is coming
    }
    PeerLocatorCloseEvent event;
    if (event_callback_) {
      event_callback_(event);
    }
  });

  ZNET_LOG_INFO("Relay client bound to {} and connected to {}", client_.local_address()->readable(),
                client_.server_address()->readable());
  return result;
}

Result PeerLocator::Disconnect() {
  return client_.Disconnect();
}

Result PeerLocator::AskPeer(std::string peer_name) {
  std::shared_ptr<PeerSession> session;
  {
    // written by the relay client's thread; AskPeer may come from any
    std::lock_guard<std::mutex> lock(mutex_);
    session = session_;
  }
  if (!session || !session->IsAlive()) {
    return Result::NotConnected;
  }
  auto pk = std::make_shared<ConnectPeerPacket>();
  pk->target_peer_ = peer_name;
  session->SendPacket(pk);
  return Result::Success;
}

void PeerLocator::Wait() {
  client_.Wait();
  task_.Wait();
}

std::string PeerLocator::peer_name() const {
  // written by the relay client's thread; readable from any
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_name_;
}

void PeerLocator::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(ZNET_BIND_FN(OnConnectEvent));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(ZNET_BIND_FN(OnDisconnectEvent));
  dispatcher.Dispatch<ClientConnectionFailedEvent>(ZNET_BIND_FN(OnConnectionFailedEvent));
}

bool PeerLocator::OnConnectEvent(ClientConnectedToServerEvent& event) {
  std::shared_ptr<PeerSession> session = event.session();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session_ = session;
  }
  session->SetCodec(BuildRendezvousCodec());
  session->SetHandler(std::make_shared<LocatorPacketHandler>(*this));
  auto identify = std::make_shared<IdentifyPacket>();
  // the private endpoints the server relays to the matched peer. The one-shot
  // locator punches from the relay connection's own socket, so its port is the
  // punch port, and every local address is a candidate at that port: which one
  // the peer shares is not knowable from here.
  auto local = client_.local_address();
  identify->punch_port_ = local ? local->port() : 0;
  if (local) {
    identify->local_endpoints_ = LocalPunchEndpoints(local->port());
  }
  session->SendPacket(identify);
  return false;
}

bool PeerLocator::OnDisconnectEvent(ClientDisconnectedFromServerEvent& event) {
  (void)event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = true;
  }
  cv_.notify_all();
  return false;
}

bool PeerLocator::OnConnectionFailedEvent(ClientConnectionFailedEvent& event) {
  (void)event;
  // the relay link died before it was ready; without this the worker would
  // wait for a disconnect event that is never coming
  FireFailed(PeerLocatorPhase::Relay, Result::CannotConnect, "");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = true;
  }
  cv_.notify_all();
  return false;
}

void PeerLocator::OnPeerNotFound(const std::string& target_peer) {
  ZNET_LOG_INFO("Relay does not know a peer named {}.", target_peer);
  // the relay link stays up; the application may ask again
  FireFailed(PeerLocatorPhase::Rendezvous, Result::PeerNotFound, target_peer);
}

void PeerLocator::FireFailed(PeerLocatorPhase phase, Result reason,
                             const std::string& target_peer) {
  PeerLocatorFailedEvent event{phase, reason, target_peer};
  if (event_callback_) {
    event_callback_(event);
  }
}

void PeerLocator::SetPeerName(std::string peer_name, std::shared_ptr<InetAddress> endpoint) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_name_ = peer_name;
    endpoint_ = endpoint;
  }
  PeerLocatorReadyEvent event{peer_name, endpoint};
  if (event_callback_) {
    event_callback_(event);
  }
}

// --- MeshLocator -------------------------------------------------------------

class MeshLocatorPacketHandler
    : public PacketHandler<MeshLocatorPacketHandler, SetPeerNamePacket,
                           StartPunchRequestPacket, PeerNotFoundPacket> {
 public:
  explicit MeshLocatorPacketHandler(MeshLocator& locator)
      : locator_(locator) {}

  void OnPacket(const SetPeerNamePacket& pk) {
    locator_.SetPeerName(pk.peer_name_, pk.endpoint_);
  }

  void OnPacket(const PeerNotFoundPacket& pk) {
    locator_.OnPeerNotFound(pk.target_peer_);
  }

  void OnPacket(const StartPunchRequestPacket& pk) {
    ZNET_LOG_INFO("Received punch request to {} at {}", pk.target_peer_,
                  pk.target_endpoint_->readable());
    std::vector<std::shared_ptr<InetAddress>> candidates;
    candidates.push_back(pk.target_endpoint_);
    for (const auto& target_private : pk.target_private_endpoints_) {
      if (target_private->readable() != pk.target_endpoint_->readable()) {
        candidates.push_back(target_private);
      }
    }
    locator_.OnPunchRequest(pk.target_peer_, std::move(candidates),
                            pk.punch_id_, pk.connection_type_);
  }

 private:
  MeshLocator& locator_;
};

namespace {

Host::Config MakeHostConfig(const MeshLocator::Config& config) {
  Host::Config out;
  out.bind_address = config.bind_address;
  out.bind_port = config.bind_port;
  out.session_options = config.session_options;
  return out;
}

ClientConfig MakeRelayClientConfig(const MeshLocator::Config& config) {
  return ClientConfig{config.server_address, config.server_port,
                      std::chrono::seconds(10), ConnectionType::TCP, {}};
}

}  // namespace

MeshLocator::MeshLocator(const Config& config)
    : config_(config),
      host_(MakeHostConfig(config)),
      client_(MakeRelayClientConfig(config)) {
  client_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

MeshLocator::~MeshLocator() {
  Disconnect();
}

Result MeshLocator::Connect() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_running_) {
      return Result::AlreadyConnected;
    }
    is_running_ = true;
    peer_name_.clear();
  }
  // the host may already be up from a previous relay stint; the mesh
  // survives relay loss, so that is not an error
  Result result = host_.Start();
  if (result != Result::Success && result != Result::AlreadyListening) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  if ((result = client_.Bind()) != Result::Success) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  if ((result = client_.Connect()) != Result::Success) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    return result;
  }
  return Result::Success;
}

Result MeshLocator::Disconnect() {
  Result result = client_.Disconnect();
  host_.Stop();
  std::lock_guard<std::mutex> lock(mutex_);
  is_running_ = false;
  relay_session_ = nullptr;
  return result;
}

Result MeshLocator::AskPeer(std::string peer_name) {
  std::shared_ptr<PeerSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session = relay_session_;
  }
  if (!session || !session->IsAlive()) {
    return Result::NotConnected;
  }
  auto pk = std::make_shared<ConnectPeerPacket>();
  pk->target_peer_ = std::move(peer_name);
  session->SendPacket(pk);
  return Result::Success;
}

void MeshLocator::Wait() {
  client_.Wait();
}

std::string MeshLocator::peer_name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_name_;
}

void MeshLocator::OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(ZNET_BIND_FN(OnConnectEvent));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(ZNET_BIND_FN(OnDisconnectEvent));
  dispatcher.Dispatch<ClientConnectionFailedEvent>(ZNET_BIND_FN(OnConnectionFailedEvent));
}

bool MeshLocator::OnConnectEvent(ClientConnectedToServerEvent& event) {
  std::shared_ptr<PeerSession> session = event.session();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    relay_session_ = session;
  }
  session->SetCodec(BuildRendezvousCodec());
  session->SetHandler(std::make_shared<MeshLocatorPacketHandler>(*this));
  auto identify = std::make_shared<IdentifyPacket>();
  identify->punch_port_ = host_.punch_port();
  // the LAN candidates are this machine's addresses with the host's UDP port
  identify->local_endpoints_ = LocalPunchEndpoints(host_.punch_port());
  session->SendPacket(identify);
  return false;
}

bool MeshLocator::OnDisconnectEvent(ClientDisconnectedFromServerEvent& event) {
  (void)event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    relay_session_ = nullptr;
  }
  // matchmaking ended; the mesh lives on
  PeerLocatorCloseEvent close_event;
  if (event_callback_) {
    event_callback_(close_event);
  }
  return false;
}

bool MeshLocator::OnConnectionFailedEvent(ClientConnectionFailedEvent& event) {
  (void)event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_running_ = false;
    relay_session_ = nullptr;
  }
  FireFailed(PeerLocatorPhase::Relay, Result::CannotConnect, "");
  PeerLocatorCloseEvent close_event;
  if (event_callback_) {
    event_callback_(close_event);
  }
  return false;
}

void MeshLocator::SetPeerName(std::string peer_name,
                              std::shared_ptr<InetAddress> endpoint) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_name_ = peer_name;
  }
  PeerLocatorReadyEvent event{peer_name, endpoint};
  if (event_callback_) {
    event_callback_(event);
  }
}

void MeshLocator::OnPeerNotFound(const std::string& target_peer) {
  ZNET_LOG_INFO("Relay does not know a peer named {}.", target_peer);
  FireFailed(PeerLocatorPhase::Rendezvous, Result::PeerNotFound, target_peer);
}

void MeshLocator::OnPunchRequest(
    std::string target_peer,
    std::vector<std::shared_ptr<InetAddress>> candidates, uint64_t punch_id,
    ConnectionType connection_type) {
  if (connection_type != ConnectionType::ZDT) {
    ZNET_LOG_ERROR(
        "Mesh punches are ZDT only; run the rendezvous with punch type zdt.");
    FireFailed(PeerLocatorPhase::Punch, Result::InvalidBackend, target_peer);
    return;
  }
  std::string self_name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    self_name = peer_name_;
  }
  const bool initiator = IsInitiator(punch_id, self_name, target_peer);
  host_.StartPunch(
      std::move(candidates), punch_id, initiator,
      std::chrono::milliseconds(5000),
      [this, punch_id, self_name, target_peer](
          Result result, std::shared_ptr<PeerSession> session) {
        if (result == Result::Success) {
          PeerConnectedEvent event{session, punch_id, self_name, target_peer};
          if (event_callback_) {
            event_callback_(event);
          }
          return;
        }
        FireFailed(PeerLocatorPhase::Punch, result, target_peer);
      });
}

void MeshLocator::FireFailed(PeerLocatorPhase phase, Result reason,
                             const std::string& target_peer) {
  PeerLocatorFailedEvent event{phase, reason, target_peer};
  if (event_callback_) {
    event_callback_(event);
  }
}

}
}