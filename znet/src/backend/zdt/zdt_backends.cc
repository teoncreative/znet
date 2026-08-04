//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//
// ZDT (znet Datagram Transport). See znet/backends/zdt.h for the overview.
//

#include "znet/backends/zdt/zdt_backends.h"

#include "znet/error.h"
#include "znet/logger.h"
#include "znet/util.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace znet {
namespace backends {

using steady_clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// ZDTClientBackend
// ---------------------------------------------------------------------------

ZDTClientBackend::ZDTClientBackend(std::shared_ptr<InetAddress> server_address,
                                   const SessionOptions& options)
    : server_address_(std::move(server_address)), config_(options.zdt),
      session_options_(options) {}

ZDTClientBackend::~ZDTClientBackend() {
  ZNET_LOG_DEBUG("Destructor of the ZDT client backend is called.");
  // Close() first so the FIN still goes out, then join: the loop reads socket_
  // and inbox_, both of which are about to be destroyed.
  Close();
  StopReceiving();
}

Result ZDTClientBackend::BindTo(const InetAddress& address) {
  socket_ = std::make_shared<UDPSocket>();
  Result result = socket_->Open(server_address_->ipv());
  if (result != Result::Success) {
    return result;
  }
  socket_->SetBlocking(false);
  socket_->SetDontFragment(true);  // make the handshake MTU probe meaningful
  ApplySocketBufferSizes(*socket_, config_.socket_recv_buffer,
                         config_.socket_send_buffer);
  result = socket_->Bind(address);
  if (result != Result::Success) {
    return result;
  }
  local_address_ = socket_->local_address();  // resolves an auto-assigned port
  is_bind_ = true;
  return Result::Success;
}

Result ZDTClientBackend::Bind() {
  auto any = InetAddress::from(GetAnyBindAddress(server_address_->ipv()), 0);
  return BindTo(*any);
}

Result ZDTClientBackend::Bind(const std::string& ip, PortNumber port) {
  auto address = InetAddress::from(ip, port);
  if (!address || !address->is_valid()) {
    return Result::InvalidAddress;
  }
  return BindTo(*address);
}

Result ZDTClientBackend::Handshake(ZDTConnection& out) {
  ZDTCookie cookie{};
  uint32_t epoch = 0;
  uint16_t negotiated_mtu = 0;
  uint64_t server_guid = 0;
  bool got_reply1 = false;

  uint8_t buf[ZNET_MAX_BUFFER_SIZE];

  // phase 1: OpenConnectionRequest1 -> OpenConnectionReply1, walking the MTU
  // ladder. The request is padded to the candidate MTU so the padded datagram
  // itself probes the path.
  for (uint16_t rung : config_.mtu_ladder) {
    for (int attempt = 0;
         attempt < config_.handshake_retries_per_rung && !got_reply1; attempt++) {
      // the rung is a link MTU; what goes in the datagram is the payload that
      // fits inside it once the IP and UDP headers are accounted for
      const uint16_t probe =
          ZDTPayloadForLinkMTU(rung, server_address_->ipv());
      Buffer request(Endianness::BigEndian);
      WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest1);
      request.WriteInt<uint8_t>(kZDTProtocolVersion);
      if (request.size() < probe) {
        std::vector<uint8_t> padding(probe - request.size(), 0);
        request.Write(padding.data(), padding.size());
      }
      if (!socket_->SendTo(*server_address_, request.data(), request.size())) {
        break;  // datagram too big for the path (DF set) -> drop to next rung
      }

      auto deadline = steady_clock::now() + config_.handshake_retransmit;
      while (steady_clock::now() < deadline && !got_reply1) {
        size_t len = 0;
        std::shared_ptr<InetAddress> from;
        RecvResult r = socket_->RecvFrom(buf, sizeof(buf), len, from);
        if (r == RecvResult::WouldBlock) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (r == RecvResult::Error) {
          break;
        }
        if (len == 0 || (buf[0] & kFlagOnline)) {
          continue;
        }
        Buffer reply(reinterpret_cast<const char*>(buf), len,
                     Endianness::BigEndian);
        ZDTOfflineMsg id;
        if (!ReadOfflineHeader(reply, id)) {
          continue;
        }
        if (id == ZDTOfflineMsg::OpenConnectionReply1) {
          server_guid = reply.ReadInt<uint64_t>();
          negotiated_mtu = reply.ReadInt<uint16_t>();
          uint8_t cookie_len = reply.ReadInt<uint8_t>();
          if (cookie_len != kZDTCookieLen) {
            continue;
          }
          reply.Read(cookie.data(), cookie.size());
          epoch = reply.ReadInt<uint32_t>();
          got_reply1 = true;
        } else if (id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
          return Result::IncompatibleVersion;
        } else if (id == ZDTOfflineMsg::NoFreeConnections) {
          return Result::ServerFull;
        }
      }
    }
    if (got_reply1) {
      break;
    }
  }
  if (!got_reply1) {
    return Result::Timeout;
  }

  // phase 2: OpenConnectionRequest2 (echo the cookie) -> OpenConnectionReply2.
  for (int attempt = 0; attempt < config_.max_retries; attempt++) {
    Buffer request(Endianness::BigEndian);
    WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest2);
    request.WriteInt<uint8_t>(static_cast<uint8_t>(cookie.size()));
    request.Write(cookie.data(), cookie.size());
    request.WriteInt<uint32_t>(epoch);
    request.WriteInetAddress(*server_address_);
    request.WriteInt<uint16_t>(negotiated_mtu);
    request.WriteInt<uint64_t>(guid_);
    socket_->SendTo(*server_address_, request.data(), request.size());

    auto deadline = steady_clock::now() + config_.handshake_retransmit;
    while (steady_clock::now() < deadline) {
      size_t len = 0;
      std::shared_ptr<InetAddress> from;
      RecvResult r = socket_->RecvFrom(buf, sizeof(buf), len, from);
      if (r == RecvResult::WouldBlock) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (r == RecvResult::Error) {
        break;
      }
      if (len == 0 || (buf[0] & kFlagOnline)) {
        continue;
      }
      Buffer reply(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
      ZDTOfflineMsg id;
      if (!ReadOfflineHeader(reply, id)) {
        continue;
      }
      if (id == ZDTOfflineMsg::OpenConnectionReply2) {
        uint64_t reply_server_guid = reply.ReadInt<uint64_t>();
        auto external = reply.ReadInetAddress();
        uint16_t mtu = reply.ReadInt<uint16_t>();
        (void)external;
        out.mtu = mtu ? mtu : negotiated_mtu;
        out.local_guid = guid_;
        out.remote_guid = reply_server_guid ? reply_server_guid : server_guid;
        return Result::Success;
      }
      if (id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
        return Result::IncompatibleVersion;
      }
      if (id == ZDTOfflineMsg::NoFreeConnections) {
        return Result::ServerFull;
      }
    }
  }
  return Result::Timeout;
}

Result ZDTClientBackend::Connect() {
  if (client_session_ && client_session_->IsAlive()) {
    return Result::AlreadyConnected;
  }
  if (!server_address_ || !server_address_->is_valid()) {
    return Result::InvalidRemoteAddress;
  }
  if (!is_bind_) {
    ZNET_LOG_ERROR(
        "Cannot connect because the ZDT client is not bound, call Bind() first.");
    return Result::CannotBind;
  }
  guid_ = GenerateGuid();
  ZDTConnection connection;
  Result result = Handshake(connection);
  if (result != Result::Success) {
    ZNET_LOG_ERROR("ZDT handshake with {} failed: {}", server_address_->readable(),
                   GetResultString(result));
    return result;
  }
  ZNET_LOG_DEBUG("ZDT connected to {} (mtu={})", server_address_->readable(),
                 connection.mtu);
  // the receive thread owns the socket from here, so the transport takes its
  // datagrams from the inbox instead of polling alongside it
  inbox_ = std::make_shared<ZDTInbox>();
  auto transport = std::make_unique<ZDTTransportLayer>(
      socket_, server_address_, config_, /*drains_own_socket=*/false, inbox_,
      connection, session_options_.common);
  client_session_ = std::make_shared<PeerSession>(
      local_address_, server_address_, std::move(transport), ConnectionType::ZDT,
      /*is_initiator=*/true, /*self_managed=*/false, session_options_);
  // blocking with a timeout: a datagram returns at once, and the timeout only
  // exists so the loop notices shutdown.
  socket_->SetBlocking(true);
  socket_->SetReceiveTimeout(std::chrono::milliseconds(200));
  {
    std::lock_guard<std::mutex> lock(receive_thread_mutex_);
    receiving_ = true;
    receive_thread_ = std::thread([this]() { ReceiveLoop(); });
  }
  return Result::Success;
}

void ZDTClientBackend::ReceiveLoop() {
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  while (receiving_.load(std::memory_order_relaxed)) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    RecvResult result = socket_->RecvFrom(buffer, sizeof(buffer), len, from);
    if (result == RecvResult::WouldBlock) {
      continue;  // receive timeout expired, just re-check the stop flag
    }
    if (result == RecvResult::Error) {
      break;  // socket closed underneath us, shutdown is in progress
    }
    if (len == 0 || !from) {
      continue;
    }
    // one peer, so anything from elsewhere is noise on the port
    if (!(*from == *server_address_)) {
      continue;
    }
    inbox_->Push(buffer, len, config_.max_inbox_datagrams);
    if (on_data_) {
      on_data_();  // the session has work; do not make it wait out its tick
    }
  }
}

void ZDTClientBackend::StopReceiving() {
  std::lock_guard<std::mutex> lock(receive_thread_mutex_);
  receiving_ = false;
  // the loop is parked in recvfrom and would otherwise hold shutdown for a
  // whole receive timeout. only safe once anything outbound has been sent.
  if (socket_ && socket_->IsValid()) {
    ShutdownSocket(socket_->handle());
  }
  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
}

Result ZDTClientBackend::Close() {
  if (!client_session_) {
    if (socket_) {
      socket_->Close();
    }
    return Result::AlreadyClosed;
  }
  return client_session_->Close();
}

void ZDTClientBackend::Update() {}

bool ZDTClientBackend::IsAlive() {
  return client_session_ && client_session_->IsAlive();
}

// ---------------------------------------------------------------------------
// ZDTServerBackend
// ---------------------------------------------------------------------------

ZDTServerBackend::ZDTServerBackend(std::shared_ptr<InetAddress> bind_address,
                                   const SessionOptions& child_options,
                                   const ServerOptions& server_options)
    : bind_address_(std::move(bind_address)), config_(child_options.zdt),
      child_session_options_(child_options), admission_(server_options) {}

ZDTServerBackend::~ZDTServerBackend() {
  ZNET_LOG_DEBUG("Destructor of the ZDT server backend is called.");
  Close();
}

Result ZDTServerBackend::Bind() {
  if (is_bind_) {
    return Result::AlreadyBound;
  }
  if (!bind_address_ || !bind_address_->is_valid()) {
    return Result::InvalidAddress;
  }
  socket_ = std::make_shared<UDPSocket>();
  Result result = socket_->Open(bind_address_->ipv());
  if (result != Result::Success) {
    return result;
  }
  // the receive thread blocks in recvfrom so a datagram wakes it immediately
  // instead of waiting for the next poll. The timeout is only there to give the
  // loop a chance to notice shutdown.
  socket_->SetBlocking(true);
  socket_->SetReceiveTimeout(std::chrono::milliseconds(200));
  ApplySocketBufferSizes(*socket_, config_.socket_recv_buffer,
                         config_.socket_send_buffer);
  result = socket_->Bind(*bind_address_);
  if (result != Result::Success) {
    return result;
  }
  auto local = socket_->local_address();
  if (local) {
    bind_address_ = local;
  }
  // cookie-signing secret + server identity. RAND_bytes needs znet::Init(),
  // which Server::Bind() runs before invoking the backend.
  RAND_bytes(secret_current_.data(), static_cast<int>(secret_current_.size()));
  has_previous_secret_ = false;
  epoch_ = 0;
  last_rotation_ = steady_clock::now();
  server_guid_ = GenerateGuid();
  is_bind_ = true;
  ZNET_LOG_DEBUG("ZDT bind to: {}", bind_address_->readable());
  return Result::Success;
}

Result ZDTServerBackend::Listen() {
  if (is_listening_) {
    return Result::AlreadyListening;
  }
  if (!is_bind_) {
    ZNET_LOG_ERROR(
        "Cannot listen because the ZDT server is not bound, call Bind() first.");
    return Result::NotBound;
  }
  is_listening_ = true;
  receiving_ = true;
  receive_thread_ = std::thread([this]() { ReceiveLoop(); });
  return Result::Success;
}

void ZDTServerBackend::ReceiveLoop() {
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  while (receiving_.load(std::memory_order_relaxed)) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    RecvResult result = socket_->RecvFrom(buffer, sizeof(buffer), len, from);
    if (result == RecvResult::WouldBlock) {
      continue;  // receive timeout expired, just re-check the stop flag
    }
    if (result == RecvResult::Error) {
      break;  // socket closed underneath us, shutdown is in progress
    }
    if (len == 0 || !from) {
      continue;
    }
    RouteDatagram(buffer, len, from);
    if (on_data_) {
      on_data_();  // a session has work; do not make it wait out its tick
    }
  }
}

void ZDTServerBackend::RouteDatagram(uint8_t* data, size_t len,
                                     const std::shared_ptr<InetAddress>& from) {
  ZNET_ZDT_ENTER_DOMAIN(receive_domain_);
  std::lock_guard<std::mutex> lock(state_mutex_);
  MaybeRotateSecret();
  if (data[0] & kFlagOnline) {
    auto it = routes_.find(from->readable());
    if (it != routes_.end()) {
      it->second.inbox->Push(data, len, config_.max_inbox_datagrams);
      return;
    }
    // online datagram from an unknown address -> drop.
    ZNET_METRIC(metrics_.zdt.datagrams_unroutable++);
    return;
  }
  Buffer offline(reinterpret_cast<const char*>(data), len,
                 Endianness::BigEndian);
  HandleOffline(offline, from, len);
}

void ZDTServerBackend::MaybeRotateSecret() {
  auto now = steady_clock::now();
  if (now - last_rotation_ < config_.cookie_secret_rotation) {
    return;
  }
  secret_previous_ = secret_current_;
  has_previous_secret_ = true;
  RAND_bytes(secret_current_.data(), static_cast<int>(secret_current_.size()));
  epoch_++;
  last_rotation_ = now;
}

ZDTCookie ZDTServerBackend::CookieFor(const std::string& peer_readable,
                                      uint32_t epoch) const {
  return ComputeCookie(secret_current_.data(), secret_current_.size(),
                       peer_readable, epoch);
}

bool ZDTServerBackend::AllowHandshake(const std::string& peer_readable) {
  auto now = steady_clock::now();
  // keep the table bounded: when it grows large, drop entries whose 1s window
  // has elapsed. This is the only per-source state the server keeps, and it is
  // capped; the stateless cookie remains the primary anti-flood defense.
  if (source_rate_.size() > static_cast<size_t>(config_.max_connections) * 2) {
    for (auto it = source_rate_.begin(); it != source_rate_.end();) {
      if (now - it->second.window_start > std::chrono::seconds(1)) {
        it = source_rate_.erase(it);
      } else {
        ++it;
      }
    }
  }
  SourceRate& entry = source_rate_[peer_readable];
  if (entry.count == 0 || now - entry.window_start > std::chrono::seconds(1)) {
    entry.window_start = now;
    entry.count = 0;
  }
  entry.count++;
  return entry.count <= config_.per_source_handshake_rate;
}

void ZDTServerBackend::HandleOffline(Buffer& buffer,
                                     const std::shared_ptr<InetAddress>& from,
                                     size_t datagram_size) {
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(buffer, id)) {
    return;
  }
  // silent on every refusal: never reply to a source the rules exclude.
  // screened before the rate table too, so an excluded source cannot fill it.
  if (admission_.Screen(*from) != AdmissionControl::Verdict::Allow) {
    ZNET_METRIC(metrics_.zdt.admission_rejected++);
    return;
  }
  const std::string key = from->readable();
  if (!AllowHandshake(key)) {
    ZNET_METRIC(metrics_.zdt.rate_limited++);
    return;  // per-source handshake rate exceeded -> drop silently
  }

  if (id == ZDTOfflineMsg::OpenConnectionRequest1) {
    if (admission_.Admit(*from) != AdmissionControl::Verdict::Allow) {
      // the user-facing attempt throttle, distinct from the anti-flood rate
      // above; a Request1 is what starts a handshake, so it is the attempt
      ZNET_METRIC(metrics_.zdt.admission_rejected++);
      return;
    }
    ZNET_METRIC(metrics_.zdt.handshakes_started++);
    uint8_t version = buffer.ReadInt<uint8_t>();
    if (version != kZDTProtocolVersion) {
      ZNET_METRIC(metrics_.zdt.handshakes_rejected++);
      Buffer out(Endianness::BigEndian);
      WriteOfflineHeader(out, ZDTOfflineMsg::IncompatibleProtocolVersion);
      out.WriteInt<uint8_t>(kZDTProtocolVersion);
      out.WriteInt<uint64_t>(server_guid_);
      socket_->SendTo(*from, out.data(), out.size());
      return;
    }
    // allocate nothing here. The received size is the MTU the path carried,
    // capped to the top ladder rung.
    uint16_t mtu = static_cast<uint16_t>(std::min<size_t>(
        datagram_size,
        ZDTPayloadForLinkMTU(config_.mtu_ladder.front(), from->ipv())));
    ZDTCookie cookie = CookieFor(key, epoch_);
    Buffer out(Endianness::BigEndian);
    WriteOfflineHeader(out, ZDTOfflineMsg::OpenConnectionReply1);
    out.WriteInt<uint64_t>(server_guid_);
    out.WriteInt<uint16_t>(mtu);
    out.WriteInt<uint8_t>(static_cast<uint8_t>(cookie.size()));
    out.Write(cookie.data(), cookie.size());
    out.WriteInt<uint32_t>(epoch_);
    socket_->SendTo(*from, out.data(), out.size());
    return;
  }

  if (id == ZDTOfflineMsg::OpenConnectionRequest2) {
    uint8_t cookie_len = buffer.ReadInt<uint8_t>();
    if (cookie_len != kZDTCookieLen) {
      return;
    }
    ZDTCookie cookie{};
    buffer.Read(cookie.data(), cookie.size());
    uint32_t epoch = buffer.ReadInt<uint32_t>();
    auto target = buffer.ReadInetAddress();
    (void)target;
    uint16_t mtu = buffer.ReadInt<uint16_t>();
    uint64_t client_guid = buffer.ReadInt<uint64_t>();

    // validate the cookie against the source address (return-routability).
    bool valid = false;
    if (epoch == epoch_) {
      valid = ConstTimeEqual(cookie, CookieFor(key, epoch));
    } else if (has_previous_secret_ && epoch == epoch_ - 1) {
      valid = ConstTimeEqual(
          cookie, ComputeCookie(secret_previous_.data(),
                                secret_previous_.size(), key, epoch));
    }
    if (!valid) {
      ZNET_METRIC(metrics_.zdt.cookies_rejected++);
      // silent drop: never reply to an unvalidated address.
      return;
    }

    auto reply2 = [&]() {
      Buffer out(Endianness::BigEndian);
      WriteOfflineHeader(out, ZDTOfflineMsg::OpenConnectionReply2);
      out.WriteInt<uint64_t>(server_guid_);
      out.WriteInetAddress(*from);
      out.WriteInt<uint16_t>(mtu);
      socket_->SendTo(*from, out.data(), out.size());
    };

    // duplicate Request2 (Reply2 was lost): re-answer idempotently.
    auto existing = routes_.find(key);
    if (existing != routes_.end() && !existing->second.session.expired()) {
      reply2();
      return;
    }
    if (static_cast<int>(routes_.size()) >= config_.max_connections) {
      ZNET_METRIC(metrics_.zdt.handshakes_rejected++);
      Buffer out(Endianness::BigEndian);
      WriteOfflineHeader(out, ZDTOfflineMsg::NoFreeConnections);
      out.WriteInt<uint64_t>(server_guid_);
      socket_->SendTo(*from, out.data(), out.size());
      return;
    }

    // address proven, allocate the session now.
    auto inbox = std::make_shared<ZDTInbox>();
    ZDTConnection connection;
    connection.mtu =
        mtu ? mtu : ZDTPayloadForLinkMTU(config_.mtu_ladder.back(), from->ipv());
    connection.local_guid = server_guid_;
    connection.remote_guid = client_guid;
    auto transport = std::make_unique<ZDTTransportLayer>(
        socket_, from, config_, /*drains_own_socket=*/false, inbox, connection,
        child_session_options_.common);
    auto session = std::make_shared<PeerSession>(
        bind_address_, from, std::move(transport), ConnectionType::ZDT,
        /*is_initiator=*/false, /*self_managed=*/false,
        child_session_options_);

    Route route;
    route.session = session;
    route.inbox = inbox;
    route.peer = from;
    route.remote_guid = client_guid;
    routes_[key] = std::move(route);
    ZNET_METRIC(metrics_.connections_accepted++);
    pending_accept_.push_back(session);
    reply2();
    ZNET_LOG_DEBUG("ZDT accepted handshake from {} (mtu={})", key, connection.mtu);
    return;
  }
}

void ZDTServerBackend::StopReceiving() {
  std::lock_guard<std::mutex> lock(receive_thread_mutex_);
  receiving_ = false;
  // the loop only re-checks that flag when RecvFrom returns, so without this
  // the join below sits out a whole receive timeout. The read direction only:
  // the sessions still on this socket have their own FINs to send, and
  // Server::MainProcessor sends them after this returns.
  if (socket_) {
    ShutdownSocketRead(socket_->handle());
  }
  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
}

Result ZDTServerBackend::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_listening_ && !is_bind_) {
      return Result::AlreadyStopped;
    }
    is_listening_ = false;
    is_bind_ = false;
  }
  // stop and join before touching the tables, otherwise the receive thread is
  // still routing into them. Joined outside every lock it might be waiting on,
  // and harmless if the shutdown path already did it.
  StopReceiving();
  std::lock_guard<std::mutex> lock(state_mutex_);
  routes_.clear();
  pending_accept_.clear();
  source_rate_.clear();
  if (socket_) {
    socket_->Close();
  }
  return Result::Success;
}

void ZDTServerBackend::Update() {}

std::shared_ptr<PeerSession> ZDTServerBackend::Accept() {
  if (!is_listening_) {
    return nullptr;
  }
  // the receive thread fills routes_/pending_accept_, this only harvests them.
  std::lock_guard<std::mutex> lock(state_mutex_);
  // reap routes whose sessions have been destroyed.
  for (auto it = routes_.begin(); it != routes_.end();) {
    if (it->second.session.expired()) {
      it = routes_.erase(it);
    } else {
      ++it;
    }
  }
  ZNET_METRIC(metrics_.connections_active = routes_.size());
  if (pending_accept_.empty()) {
    return nullptr;
  }
  auto session = pending_accept_.front();
  pending_accept_.pop_front();
  return session;
}

void ZDTServerBackend::AcceptAndReject() {
  // nothing to reject at the socket level for UDP; unvalidated handshakes are
  // simply not promoted to sessions.
}

bool ZDTServerBackend::IsAlive() {
  return is_listening_;
}

}  // namespace backends
}  // namespace znet
