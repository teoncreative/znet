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
// ZDT hole-punch. See znet/backends/zdt.h for the transport itself.
//

#include "dialer_internal.h"

#include "znet/backends/zdt.h"
#include "znet/error.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace znet {
namespace p2p {

// ZDT UDP hole-punch. Both peers bind the local endpoint (SO_REUSEADDR reuses the
// just-closed rendezvous port) and spray Punch datagrams to open the mappings.
// Every candidate is sprayed from the one socket and whichever answers first
// wins, which is how two peers behind the same NAT find each other's private
// address. The post-punch handshake skips the cookie round-trip: the
// rendezvous already vouched for both peers and the punch itself proves
// return-routability.
std::shared_ptr<PeerSession> PunchSyncZDT(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& peers, Result* out_result,
    bool is_initiator, int timeout_ms) {
  using namespace backends;
  using clock = std::chrono::steady_clock;

  if (!local || !local->is_valid() || peers.empty()) {
    *out_result = Result::InvalidAddress;
    return nullptr;
  }
  for (const auto& candidate : peers) {
    if (!candidate || !candidate->is_valid()) {
      *out_result = Result::InvalidAddress;
      return nullptr;
    }
  }

  auto socket = std::make_shared<UDPSocket>();
  if (socket->Open(local->ipv()) != Result::Success) {
    *out_result = Result::CannotCreateSocket;
    return nullptr;
  }
  socket->SetBlocking(false);
  socket->SetDontFragment(true);
  if (socket->Bind(*local) != Result::Success) {
    ZNET_LOG_ERROR("ZDT punch: failed to bind {}: {}", local->readable(),
                   GetLastErrorInfo());
    *out_result = Result::CannotBind;
    return nullptr;
  }
  for (const auto& candidate : peers) {
    ZNET_LOG_INFO("ZDT punch: {} -> {} (initiator={})", local->readable(),
                  candidate->readable(), is_initiator);
  }

  ZDTOptions config;
  // the punched socket carries the whole session afterwards, so it gets the
  // same buffer treatment as the backend sockets
  ApplySocketBufferSizes(*socket, config.socket_recv_buffer,
                         config.socket_send_buffer);
  ZDTConnection connection;
  connection.mtu = 1200;  // conservative; skips the ladder probe for P2P
  connection.local_guid = GenerateGuid();

  auto build_offline = [](ZDTOfflineMsg id) {
    Buffer buffer(Endianness::BigEndian);
    WriteOfflineHeader(buffer, id);
    return buffer;
  };

  auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  auto last_punch = clock::time_point{};
  auto last_request = clock::time_point{};
  uint8_t buf[ZNET_MAX_BUFFER_SIZE];

  while (clock::now() < deadline) {
    auto now = clock::now();
    // keep the hole open from both sides, toward every candidate.
    if (now - last_punch > std::chrono::milliseconds(50)) {
      Buffer punch = build_offline(ZDTOfflineMsg::Punch);
      for (const auto& candidate : peers) {
        socket->SendTo(*candidate, punch.data(), punch.size());
      }
      last_punch = now;
    }
    // the initiator also drives the handshake (Request1 doubles as a punch).
    if (is_initiator && now - last_request > std::chrono::milliseconds(100)) {
      Buffer request = build_offline(ZDTOfflineMsg::OpenConnectionRequest1);
      request.WriteInt<uint8_t>(kZDTProtocolVersion);
      request.WriteInt<uint64_t>(connection.local_guid);
      for (const auto& candidate : peers) {
        socket->SendTo(*candidate, request.data(), request.size());
      }
      last_request = now;
    }

    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    if (socket->RecvFrom(buf, sizeof(buf), len, from) != RecvResult::Received ||
        len == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    // online data from the peer means the initiator has connected: the responder
    // adopts the actual source address and hands the datagram to a new transport.
    if (buf[0] & kFlagOnline) {
      if (is_initiator) {
        continue;  // shouldn't precede Reply2; ignore
      }
      auto transport = std::make_unique<ZDTTransportLayer>(
          socket, from, config, /*drains_own_socket=*/true, nullptr, connection);
      transport->OnDatagram(buf, len);
      *out_result = Result::Success;
      return std::make_shared<PeerSession>(local, from, std::move(transport),
                                           ConnectionType::ZDT,
                                           /*is_initiator=*/false,
                                           /*self_managed=*/true);
    }

    Buffer in(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
    ZDTOfflineMsg id;
    if (!ReadOfflineHeader(in, id) || id == ZDTOfflineMsg::Punch) {
      continue;  // stray or keepalive punch
    }

    if (!is_initiator && id == ZDTOfflineMsg::OpenConnectionRequest1) {
      uint8_t version = in.ReadInt<uint8_t>();
      if (version != kZDTProtocolVersion) {
        Buffer bad = build_offline(ZDTOfflineMsg::IncompatibleProtocolVersion);
        bad.WriteInt<uint8_t>(kZDTProtocolVersion);
        bad.WriteInt<uint64_t>(connection.local_guid);
        socket->SendTo(*from, bad.data(), bad.size());
        continue;
      }
      connection.remote_guid = in.ReadInt<uint64_t>();
      Buffer reply = build_offline(ZDTOfflineMsg::OpenConnectionReply2);
      reply.WriteInt<uint64_t>(connection.local_guid);
      reply.WriteInt<uint16_t>(connection.mtu);
      socket->SendTo(*from, reply.data(), reply.size());
      // stay in the loop until online data confirms the peer connected, so a lost
      // Reply2 is simply re-sent on the next Request1.
      continue;
    }
    if (is_initiator && id == ZDTOfflineMsg::OpenConnectionReply2) {
      connection.remote_guid = in.ReadInt<uint64_t>();
      uint16_t mtu = in.ReadInt<uint16_t>();
      if (mtu != 0) {
        connection.mtu = std::min(connection.mtu, mtu);
      }
      auto transport = std::make_unique<ZDTTransportLayer>(
          socket, from, config, /*drains_own_socket=*/true, nullptr, connection);
      *out_result = Result::Success;
      return std::make_shared<PeerSession>(local, from, std::move(transport),
                                           ConnectionType::ZDT,
                                           /*is_initiator=*/true,
                                           /*self_managed=*/true);
    }
    if (is_initiator && id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
      *out_result = Result::IncompatibleVersion;
      return nullptr;
    }
  }

  *out_result = Result::Timeout;
  return nullptr;
}

}  // namespace p2p
}  // namespace znet
