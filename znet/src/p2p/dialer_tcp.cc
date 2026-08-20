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
#include "znet/detail/socket_ops.h"
#include "znet/error.h"
#include "znet/transport.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace znet {
namespace p2p {
namespace {

int LastErr() {
#ifdef ZNET_TARGET_WIN
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool WouldBlock(int e) {
#ifdef ZNET_TARGET_WIN
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
  return e == EINPROGRESS || e == EWOULDBLOCK || e == EALREADY;
#endif
}

#if defined(ZNET_TARGET_WIN)
// On Windows the parameter is ignored
constexpr int ToSelectNFDS(SocketHandle) { return 0; }
#else
constexpr int ToSelectNFDS(SocketHandle handle) { return handle + 1; }
#endif

}  // namespace

namespace {

// one socket/bind/connect cycle, waited on until `deadline`. A refused or
// failed connect closes the socket and reports; the caller decides whether
// How long a freshly opened connection gets to prove it is paired. A real one
// is ready in a millisecond or two on loopback, so this only has to cover a
// slow link, not a slow handshake.
ZNET_INLINE_CONSTEXPR std::chrono::milliseconds kPairingConfirmation{500};

// The session drives itself, so waiting is all this takes.
bool WaitForPairing(const std::shared_ptr<PeerSession>& session,
                    std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    if (session->IsReady()) {
      return true;
    }
    if (!session->IsAlive()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return session->IsReady();
}

// the budget allows another cycle. `log_bind_failure` is off while the caller
// is retrying an already-reported bind failure, so a stuck port reports once
// instead of thousands of times over the budget.
std::shared_ptr<PeerSession> TryPunchTCPOnce(
    const std::shared_ptr<InetAddress>& local,
    const std::shared_ptr<InetAddress>& peer,
    std::chrono::steady_clock::time_point deadline, bool is_initiator,
    bool log_bind_failure, Result* out_result) {
  const int domain = GetDomainByInetProtocolVersion(local->ipv());
  SocketHandle socket_handle = socket(domain, SOCK_STREAM, 0);
  if (!IsValidSocketHandle(socket_handle)) {
    *out_result = Result::CannotCreateSocket;
    return nullptr;
  }

  int option = 1;
  setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option), sizeof(option));
#ifndef ZNET_TARGET_WIN
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
    if (log_bind_failure) {
      ZNET_LOG_ERROR("Failed to bind socket to {}: {} ({})", local->readable(),
                     LastErr(), GetLastErrorInfo());
    }
    CloseSocket(socket_handle);
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
#ifdef ZNET_TARGET_WIN
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
  // retries run every few milliseconds for the whole budget, so per-attempt
  // logging is thousands of identical lines; each candidate reports a reason
  // only when it changes
  std::vector<Result> last_logged(peers.size(), Result::Success);
  size_t attempt = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const size_t candidate = attempt % peers.size();
    const auto& peer = peers[candidate];
    attempt++;
    Result attempt_result = Result::Failure;
    const auto attempt_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + attempt_cap);
    auto session = TryPunchTCPOnce(local, peer, attempt_deadline, is_initiator,
                                   last_logged[candidate] != Result::CannotBind,
                                   &attempt_result);
    if (session) {
      // A simultaneous open can also complete against a socket the peer has
      // already given up on, and connect() cannot tell the two apart. The
      // handshake can: a paired session goes ready almost at once, while an
      // orphaned one never does. Confirming here keeps the retry inside the
      // punch budget, rather than handing the caller a connection that will
      // never carry anything.
      if (WaitForPairing(session, std::min(deadline, std::chrono::steady_clock::now() +
                                                         kPairingConfirmation))) {
        *out_result = Result::Success;
        return session;
      }
      ZNET_LOG_DEBUG("Punch to {} opened but never paired, retrying.",
                     peer->readable());
      session->Close();
      last_result = Result::Timeout;
      continue;
    }
    last_result = attempt_result;
    if (attempt_result == Result::CannotCreateSocket) {
      break;  // out of sockets; another cycle buys nothing
    }
    if (attempt_result == Result::Timeout &&
        std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    if (attempt_result != last_logged[candidate]) {
      last_logged[candidate] = attempt_result;
      ZNET_LOG_DEBUG("Punch attempt to {} failed ({}), retrying.",
                     peer->readable(), GetResultString(attempt_result));
    }
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
