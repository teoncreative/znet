//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/host.h"

#include "znet/backends/zdt.h"
#include "znet/error.h"
#include "znet/logger.h"

#include <algorithm>
#include <thread>

namespace znet {
namespace p2p {

using namespace backends;
using clock = std::chrono::steady_clock;

namespace {

Buffer BuildOffline(ZDTOfflineMsg id) {
  Buffer buffer(Endianness::BigEndian);
  WriteOfflineHeader(buffer, id);
  return buffer;
}

}  // namespace

Host::Host(const Config& config) : config_(config) {}

Host::~Host() {
  Stop();
}

Result Host::Start() {
  if (running_.load()) {
    return Result::AlreadyListening;
  }
  auto bind_address = InetAddress::from(config_.bind_address, config_.bind_port);
  if (!bind_address || !bind_address->is_valid() ||
      bind_address->ipv() == InetProtocolVersion::Unix) {
    return Result::InvalidAddress;
  }
  socket_ = std::make_shared<UDPSocket>();
  if (socket_->Open(bind_address->ipv()) != Result::Success) {
    return Result::CannotCreateSocket;
  }
  socket_->SetBlocking(false);
  socket_->SetDontFragment(true);
  ApplySocketBufferSizes(*socket_,
                         config_.session_options.zdt.socket_recv_buffer,
                         config_.session_options.zdt.socket_send_buffer);
  if (socket_->Bind(*bind_address) != Result::Success) {
    ZNET_LOG_ERROR("P2P host: failed to bind {}: {}", bind_address->readable(),
                   GetLastErrorInfo());
    socket_->Close();
    socket_ = nullptr;
    return Result::CannotBind;
  }
  local_address_ = socket_->local_address();
  running_.store(true);
  task_.Run([this]() { TickLoop(); });
  ZNET_LOG_INFO("P2P host up on {}", local_address_->readable());
  return Result::Success;
}

void Host::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  task_.RequestStop();
  task_.Wait();
  // the tick thread is gone, so its state is safe to touch from here
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& punch : pending_) {
      punches_.push_back(std::move(punch));
    }
    pending_.clear();
  }
  for (auto& punch : punches_) {
    if (punch.on_done) {
      punch.on_done(Result::AlreadyStopped, nullptr);
    }
  }
  punches_.clear();
  for (auto& item : routes_) {
    ResolveWaiters(item.second, Result::AlreadyStopped);
    item.second.session->Close();
    item.second.session->ReleaseHandler();
  }
  routes_.clear();
  session_count_.store(0);
  if (socket_) {
    socket_->Close();
  }
}

void Host::StartPunch(std::vector<std::shared_ptr<InetAddress>> candidates,
                      uint64_t punch_id, bool is_initiator,
                      std::chrono::milliseconds timeout,
                      PunchCallback on_done) {
  Punch punch;
  punch.candidates = std::move(candidates);
  punch.is_initiator = is_initiator;
  punch.punch_id = punch_id;
  punch.connection.mtu = 1200;  // conservative; skips the ladder probe for P2P
  punch.connection.local_guid = GenerateGuid();
  punch.timeout = timeout;
  punch.deadline = clock::now() + timeout;
  punch.on_done = std::move(on_done);

  bool refused = !running_.load() || punch.candidates.empty();
  for (const auto& candidate : punch.candidates) {
    refused = refused || !candidate || !candidate->is_valid();
  }
  if (refused) {
    if (punch.on_done) {
      punch.on_done(running_.load() ? Result::InvalidAddress
                                    : Result::AlreadyStopped,
                    nullptr);
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.push_back(std::move(punch));
  }
}

void Host::TickLoop() {
  while (!task_.IsStopRequested()) {
    bool worked = DrainSocket();
    worked = TickPunches() || worked;
    worked = ProcessSessions() || worked;
    if (!worked) {
      // same doze as a self-managed session: no spinning core when idle
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

bool Host::DrainSocket() {
  bool any = false;
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  for (;;) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    if (socket_->RecvFrom(buffer, sizeof(buffer), len, from) !=
            RecvResult::Received ||
        len == 0 || !from) {
      break;
    }
    any = true;
    auto it = routes_.find(from->readable());
    if (it != routes_.end()) {
      it->second.transport->OnDatagram(buffer, len);
      continue;
    }
    HandleOffline(from, buffer, len);
  }
  return any;
}

bool Host::TickPunches() {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& punch : pending_) {
      punches_.push_back(std::move(punch));
    }
    pending_.clear();
  }
  // a punch toward a peer that already routes resolves with the live session,
  // which is what a duplicate pairing amounts to
  for (size_t i = 0; i < punches_.size();) {
    Route* existing = nullptr;
    for (const auto& candidate : punches_[i].candidates) {
      auto it = routes_.find(candidate->readable());
      if (it != routes_.end()) {
        existing = &it->second;
        break;
      }
    }
    if (existing) {
      Punch punch = std::move(punches_[i]);
      punches_.erase(punches_.begin() + static_cast<long>(i));
      if (punch.on_done) {
        if (existing->session->IsReady()) {
          punch.on_done(Result::Success, existing->session);
        } else {
          existing->waiters.push_back(std::move(punch.on_done));
        }
      }
      continue;
    }
    i++;
  }
  bool any = false;
  const auto now = clock::now();
  for (size_t i = 0; i < punches_.size();) {
    Punch& punch = punches_[i];
    if (now >= punch.deadline) {
      FailPunch(i, Result::Timeout);
      continue;  // i now names the next punch
    }
    // keep the hole open from both sides, toward every candidate
    if (now - punch.last_punch > std::chrono::milliseconds(50)) {
      Buffer datagram = BuildOffline(ZDTOfflineMsg::Punch);
      for (const auto& candidate : punch.candidates) {
        socket_->SendTo(*candidate, datagram.data(), datagram.size());
      }
      punch.last_punch = now;
      any = true;
    }
    // the initiator also drives the handshake (Request1 doubles as a punch)
    if (punch.is_initiator &&
        now - punch.last_request > std::chrono::milliseconds(100)) {
      Buffer request = BuildOffline(ZDTOfflineMsg::OpenConnectionRequest1);
      request.WriteInt<uint8_t>(kZDTProtocolVersion);
      request.WriteInt<uint64_t>(punch.connection.local_guid);
      for (const auto& candidate : punch.candidates) {
        socket_->SendTo(*candidate, request.data(), request.size());
      }
      punch.last_request = now;
      any = true;
    }
    i++;
  }
  return any;
}

bool Host::ProcessSessions() {
  bool any = false;
  const auto now = clock::now();
  for (auto it = routes_.begin(); it != routes_.end();) {
    auto& route = it->second;
    auto& session = route.session;
    if (!session->IsAlive()) {
      // dying before the handshake finished is a failed punch to whoever asked
      ResolveWaiters(route, Result::CannotConnect);
      // the same teardown a server worker does; see CleanupAndProcessSessions
      session->ReleaseHandler();
      it = routes_.erase(it);
      continue;
    }
    any = session->Process() || any;
    // checked after Process(), which is what advances the handshake
    if (!route.waiters.empty()) {
      if (session->IsReady()) {
        ZNET_LOG_INFO("P2P host: session with {} is ready",
                      session->remote_address()->readable());
        ResolveWaiters(route, Result::Success);
      } else if (now >= route.ready_deadline) {
        ZNET_LOG_WARN("P2P host: {} punched but never finished its handshake",
                      session->remote_address()->readable());
        ResolveWaiters(route, Result::Timeout);
        session->Close();
        session->ReleaseHandler();
        it = routes_.erase(it);
        continue;
      }
    }
    ++it;
  }
  session_count_.store(routes_.size(), std::memory_order_relaxed);
  return any;
}

void Host::ResolveWaiters(Route& route, Result result) {
  // moved out first: a callback may punch again and reach this route
  std::vector<PunchCallback> waiters;
  waiters.swap(route.waiters);
  for (auto& waiter : waiters) {
    if (waiter) {
      waiter(result, result == Result::Success ? route.session : nullptr);
    }
  }
}

void Host::HandleOffline(const std::shared_ptr<InetAddress>& from,
                         const uint8_t* data, size_t len) {
  // attribution is by source address: the punch whose candidate list holds
  // the sender. A symmetric NAT rewriting ports defeats this, but it defeats
  // the punch itself first.
  size_t index = punches_.size();
  for (size_t i = 0; i < punches_.size(); i++) {
    for (const auto& candidate : punches_[i].candidates) {
      if (candidate->readable() == from->readable()) {
        index = i;
        break;
      }
    }
    if (index != punches_.size()) {
      break;
    }
  }
  if (index == punches_.size()) {
    return;  // a stray, or a peer whose punch already resolved
  }
  Punch& punch = punches_[index];

  // online data means the initiator considers itself connected: the responder
  // adopts the source and hands the datagram to the new transport
  if ((data[0] & kFlagOnline) != 0) {
    if (punch.is_initiator) {
      return;  // shouldn't precede Reply2; ignore
    }
    CompletePunch(index, from, data, len);
    return;
  }

  Buffer in(reinterpret_cast<const char*>(data), len, Endianness::BigEndian);
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(in, id) || id == ZDTOfflineMsg::Punch) {
    return;  // stray or keepalive punch
  }

  if (!punch.is_initiator && id == ZDTOfflineMsg::OpenConnectionRequest1) {
    uint8_t version = in.ReadInt<uint8_t>();
    if (version != kZDTProtocolVersion) {
      Buffer bad = BuildOffline(ZDTOfflineMsg::IncompatibleProtocolVersion);
      bad.WriteInt<uint8_t>(kZDTProtocolVersion);
      bad.WriteInt<uint64_t>(punch.connection.local_guid);
      socket_->SendTo(*from, bad.data(), bad.size());
      return;
    }
    punch.connection.remote_guid = in.ReadInt<uint64_t>();
    Buffer reply = BuildOffline(ZDTOfflineMsg::OpenConnectionReply2);
    reply.WriteInt<uint64_t>(punch.connection.local_guid);
    reply.WriteInt<uint16_t>(punch.connection.mtu);
    socket_->SendTo(*from, reply.data(), reply.size());
    // stays pending until online data confirms the peer connected, so a lost
    // Reply2 is simply re-answered on the next Request1
    return;
  }
  if (punch.is_initiator && id == ZDTOfflineMsg::OpenConnectionReply2) {
    punch.connection.remote_guid = in.ReadInt<uint64_t>();
    uint16_t mtu = in.ReadInt<uint16_t>();
    if (mtu != 0) {
      punch.connection.mtu = std::min(punch.connection.mtu, mtu);
    }
    CompletePunch(index, from, nullptr, 0);
    return;
  }
  if (punch.is_initiator && id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
    FailPunch(index, Result::IncompatibleVersion);
  }
}

void Host::CompletePunch(size_t index, const std::shared_ptr<InetAddress>& from,
                         const uint8_t* first_datagram, size_t len) {
  Punch punch = std::move(punches_[index]);
  punches_.erase(punches_.begin() + static_cast<long>(index));

  auto inbox = std::make_shared<ZDTInbox>();
  auto transport = std::make_unique<ZDTTransportLayer>(
      socket_, from, config_.session_options.zdt, /*drains_own_socket=*/false,
      inbox, punch.connection, config_.session_options.common);
  ZDTTransportLayer* raw = transport.get();
  if (first_datagram != nullptr) {
    raw->OnDatagram(first_datagram, len);
  }
  auto session = std::make_shared<PeerSession>(
      local_address_, from, std::move(transport), ConnectionType::ZDT,
      punch.is_initiator, /*self_managed=*/false, config_.session_options);
  Route route;
  route.transport = raw;
  route.session = session;
  route.ready_deadline = clock::now() + punch.timeout;
  if (punch.on_done) {
    route.waiters.push_back(std::move(punch.on_done));
  }
  routes_[from->readable()] = std::move(route);
  session_count_.store(routes_.size(), std::memory_order_relaxed);
  // ProcessSessions resolves the waiters once the handshake lands.
  ZNET_LOG_INFO("P2P host: punched {} (punch id {}), handshaking",
                from->readable(), punch.punch_id);
}

void Host::FailPunch(size_t index, Result reason) {
  Punch punch = std::move(punches_[index]);
  punches_.erase(punches_.begin() + static_cast<long>(index));
  ZNET_LOG_WARN("P2P host: punch {} failed: {}", punch.punch_id,
                GetResultString(reason));
  if (punch.on_done) {
    punch.on_done(reason, nullptr);
  }
}

}  // namespace p2p
}  // namespace znet
