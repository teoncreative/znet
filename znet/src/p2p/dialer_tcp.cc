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
// TCP hole-punch: simultaneous connect() from both peers, driven by select().
//

#include "dialer_internal.h"

#include "znet/backends/tcp.h"
#include "znet/error.h"
#include "znet/transport.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace znet {
namespace p2p {
namespace {

int LastErr() {
#ifdef TARGET_WIN
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool WouldBlock(int e) {
#ifdef TARGET_WIN
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
  return e == EINPROGRESS || e == EWOULDBLOCK || e == EALREADY;
#endif
}

#if defined(TARGET_WIN)
// On Windows the parameter is ignored
constexpr int ToSelectNFDS(SocketHandle) { return 0; }
#else
constexpr int ToSelectNFDS(SocketHandle handle) { return handle + 1; }
#endif

}  // namespace

namespace {

// one socket/bind/connect cycle, waited on until `deadline`. A refused or
// failed connect closes the socket and reports; the caller decides whether
// the budget allows another cycle.
std::shared_ptr<PeerSession> TryPunchTCPOnce(
    const std::shared_ptr<InetAddress>& local,
    const std::shared_ptr<InetAddress>& peer,
    std::chrono::steady_clock::time_point deadline, bool is_initiator,
    Result* out_result) {
  const int domain = GetDomainByInetProtocolVersion(local->ipv());
  SocketHandle socket_handle = socket(domain, SOCK_STREAM, 0);
  if (!IsValidSocketHandle(socket_handle)) {
    *out_result = Result::CannotCreateSocket;
    return nullptr;
  }

  int option = 1;
  setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option), sizeof(option));
#ifndef TARGET_WIN
  // the punch rebinds the port the relay connection just released, and that
  // socket had SO_REUSEPORT: the kernel refuses the bind to a matching bucket
  // unless this one carries it too, which intermittently ate the whole punch
  // budget in EADDRINUSE retries
  setsockopt(socket_handle, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&option), sizeof(option));
#endif

  if (local->ipv() == InetProtocolVersion::IPv6) {
    setsockopt(socket_handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&option), sizeof(option));
  }

  if (bind(socket_handle, local->handle_ptr(), local->addr_size()) != 0) {
    CloseSocket(socket_handle);
    ZNET_LOG_ERROR("Failed to bind socket to {}: {} ({})", local->readable(), LastErr(), GetLastErrorInfo());
    *out_result = Result::CannotBind;
    return nullptr;
  }

  SetSocketBlocking(socket_handle, false);
  SetTCPNoDelay(socket_handle);

  if (connect(socket_handle, peer->handle_ptr(), peer->addr_size()) != 0 &&
      !WouldBlock(LastErr())) {
    CloseSocket(socket_handle);
    *out_result = Result::CannotConnect;
    return nullptr;
  }

  sockaddr_storage local_ss{};
  socklen_t local_len = sizeof(local_ss);
  if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&local_ss), &local_len) == 0) {
    auto confirm_address = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
    if (confirm_address) {
      ZNET_LOG_DEBUG("getsockname: {}", confirm_address->readable());
    } else {
      ZNET_LOG_DEBUG("getsockname: invalid address");
    }
  } else {
    ZNET_LOG_ERROR("getsockname failed: {}", GetLastErrorInfo());
  }

  while (true) {
    if (std::chrono::steady_clock::now() >= deadline) {
      CloseSocket(socket_handle);
      *out_result = Result::Timeout;
      return nullptr;
    }

    fd_set write_set;
    fd_set error_set;
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    FD_SET(socket_handle, &write_set);
    FD_SET(socket_handle, &error_set);

    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200ms polling interval

    int result = select(ToSelectNFDS(socket_handle), nullptr, &write_set, &error_set, &tv);
    if (result < 0) {
#ifdef TARGET_WIN
      if (LastErr() == WSAEINTR) {
        continue;
      }
#else
      if (errno == EINTR) {
        continue;
      }
#endif
      CloseSocket(socket_handle);
      *out_result = Result::Failure;
      return nullptr;
    }

    if (result == 0) {
      continue;
    }

    if (FD_ISSET(socket_handle, &write_set) || FD_ISSET(socket_handle, &error_set)) {
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length);
      if (socket_error == 0) {
        SetSocketBlocking(socket_handle, true);
        *out_result = Result::Success;
        return std::make_shared<PeerSession>(local, peer,
                                             std::make_unique<backends::TCPTransportLayer>(socket_handle),
                                             ConnectionType::TCP,
                                             is_initiator,
                                             true);
      }
      CloseSocket(socket_handle);
      *out_result = Result::CannotConnect;
      return nullptr;
    }
  }
}

}  // namespace

std::shared_ptr<PeerSession> PunchSyncTCP(const std::shared_ptr<InetAddress>& local,
                                       const std::vector<std::shared_ptr<InetAddress>>& peers,
                                       Result* out_result,
                                       bool is_initiator,
                                       int timeout_ms) {
  if (!local || !local->is_valid() || peers.empty()) {
    *out_result = Result::InvalidAddress;
    return nullptr;
  }
  for (const auto& candidate : peers) {
    if (!candidate || !candidate->is_valid()) {
      *out_result = Result::InvalidAddress;
      return nullptr;
    }
    ZNET_LOG_INFO("Attempting to punch to {} from {}", candidate->readable(),
                  local->readable());
  }

  // a TCP punch is a simultaneous open, and a SYN that lands before the peer
  // has bound its port is answered with RST. One attempt is a coin flip on
  // startup skew; retrying inside the budget is what makes it a punch.
  // Candidates rotate across attempts, and each attempt is capped so one that
  // blackholes its SYN (a private address on some other network) cannot eat
  // the whole budget.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  const auto attempt_cap = std::chrono::milliseconds(
      peers.size() > 1 ? 1000 : timeout_ms);
  Result last_result = Result::Timeout;
  size_t attempt = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto& peer = peers[attempt % peers.size()];
    attempt++;
    Result attempt_result = Result::Failure;
    const auto attempt_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + attempt_cap);
    auto session = TryPunchTCPOnce(local, peer, attempt_deadline, is_initiator,
                                   &attempt_result);
    if (session) {
      *out_result = Result::Success;
      return session;
    }
    last_result = attempt_result;
    if (attempt_result == Result::CannotCreateSocket) {
      break;  // out of sockets; another cycle buys nothing
    }
    if (attempt_result == Result::Timeout &&
        std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    ZNET_LOG_DEBUG("Punch attempt to {} failed ({}), retrying.",
                   peer->readable(), GetResultString(attempt_result));
    // a failed attempt dies the moment the RST lands, so the window in which
    // the two SYNs can cross is tiny. Retrying fast keeps the windows dense,
    // and the asymmetric cadence keeps two equally-paced loops from settling
    // into antiphase and missing each other for the whole budget.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(is_initiator ? 1 : 3));
  }
  *out_result = last_result;
  return nullptr;
}

}  // namespace p2p
}  // namespace znet
