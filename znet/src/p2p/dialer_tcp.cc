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

// How long one round of concurrent connects waits before starting a fresh
// one. A simultaneous open only completes when both SYNs are in flight
// together, so a round that finds nobody home is worth abandoning quickly:
// what matters is how often every candidate gets a SYN, not how long any one
// attempt is allowed to hang.
ZNET_INLINE_CONSTEXPR std::chrono::milliseconds kRoundCap{250};

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

// One in-flight connect toward one candidate.
struct Attempt {
  SocketHandle handle;
  std::shared_ptr<InetAddress> peer;
};

// Every attempt in a round binds the same local port, because the punch has to
// reuse that port's NAT mapping. That is only allowed with the reuse flags
// below, and it is why a round can race candidates at all.
SocketHandle OpenPunchSocket(const std::shared_ptr<InetAddress>& local,
                             bool log_bind_failure, Result* out_result) {
  const int domain = GetDomainByInetProtocolVersion(local->ipv());
  SocketHandle handle = socket(domain, SOCK_STREAM, 0);
  if (!IsValidSocketHandle(handle)) {
    *out_result = Result::CannotCreateSocket;
    return kSocketInvalid;
  }

  int option = 1;
  setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option), sizeof(option));
#ifndef ZNET_TARGET_WIN
  // the punch rebinds the port the relay connection just released, and that
  // socket had SO_REUSEPORT: the kernel refuses the bind to a matching bucket
  // unless this one carries it too, which intermittently ate the whole punch
  // budget in EADDRINUSE retries
  setsockopt(handle, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&option), sizeof(option));
#endif

  if (local->ipv() == InetProtocolVersion::IPv6) {
    setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&option), sizeof(option));
  }

  if (bind(handle, local->handle_ptr(), local->addr_size()) != 0) {
    if (log_bind_failure) {
      ZNET_LOG_ERROR("Failed to bind socket to {}: {} ({})", local->readable(),
                     LastErr(), GetLastErrorInfo());
    }
    CloseSocket(handle);
    *out_result = Result::CannotBind;
    return kSocketInvalid;
  }

  SetSocketBlocking(handle, false);
  SetTCPNoDelay(handle);
  return handle;
}

// Races every candidate at once and returns the first connection that opens.
//
// Walking candidates one at a time gives each of them only its turn's share of
// the round trips, and a punch needs the SYNs dense on the candidate that can
// actually answer, which is not knowable in advance. So they all go together,
// a refusal drops that candidate from the round rather than the round from the
// budget, and the round ends as soon as one opens or all of them refuse.
std::shared_ptr<PeerSession> RaceCandidatesOnce(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& peers,
    std::chrono::steady_clock::time_point deadline, bool is_initiator,
    bool log_bind_failure, Result* out_result) {
  std::vector<Attempt> attempts;
  attempts.reserve(peers.size());
  *out_result = Result::CannotConnect;

  for (const auto& peer : peers) {
    Result open_result = Result::Success;
    SocketHandle handle = OpenPunchSocket(local, log_bind_failure, &open_result);
    if (!IsValidSocketHandle(handle)) {
      *out_result = open_result;
      if (open_result == Result::CannotCreateSocket) {
        break;  // out of descriptors; the rest of the round buys nothing
      }
      continue;
    }
    if (connect(handle, peer->handle_ptr(), peer->addr_size()) != 0 &&
        !WouldBlock(LastErr())) {
      CloseSocket(handle);
      continue;  // refused outright, which for a punch just means "not yet"
    }
    attempts.push_back(Attempt{handle, peer});
  }

  auto close_all = [&attempts]() {
    for (const auto& attempt : attempts) {
      CloseSocket(attempt.handle);
    }
    attempts.clear();
  };

  while (!attempts.empty() && std::chrono::steady_clock::now() < deadline) {
    fd_set write_set;
    fd_set error_set;
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    SocketHandle highest = attempts.front().handle;
    for (const auto& attempt : attempts) {
      FD_SET(attempt.handle, &write_set);
      FD_SET(attempt.handle, &error_set);
      if (attempt.handle > highest) {
        highest = attempt.handle;
      }
    }

    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;  // 20ms, so a round reacts without busy waiting
    const int ready = select(ToSelectNFDS(highest), nullptr, &write_set,
                             &error_set, &tv);
    if (ready < 0) {
#ifdef ZNET_TARGET_WIN
      if (LastErr() == WSAEINTR) {
        continue;
      }
#else
      if (errno == EINTR) {
        continue;
      }
#endif
      close_all();
      *out_result = Result::Failure;
      return nullptr;
    }
    if (ready == 0) {
      continue;
    }

    for (size_t i = 0; i < attempts.size();) {
      const Attempt& attempt = attempts[i];
      if (!FD_ISSET(attempt.handle, &write_set) &&
          !FD_ISSET(attempt.handle, &error_set)) {
        i++;
        continue;
      }
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      getsockopt(attempt.handle, SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&socket_error), &length);
      if (socket_error != 0) {
        CloseSocket(attempt.handle);
        attempts.erase(attempts.begin() + static_cast<long>(i));
        continue;  // this candidate is not home yet; the others race on
      }

      const SocketHandle winner = attempt.handle;
      std::shared_ptr<InetAddress> peer = attempt.peer;
      attempts.erase(attempts.begin() + static_cast<long>(i));
      close_all();  // one pairing per punch; the rest were never answered
      SetSocketBlocking(winner, true);
      *out_result = Result::Success;
      return std::make_shared<PeerSession>(
          local, peer, std::make_unique<backends::TCPTransportLayer>(winner),
          ConnectionType::TCP, is_initiator, true);
    }
  }

  const bool refused_every_candidate = attempts.empty();
  close_all();
  if (!refused_every_candidate && *out_result == Result::CannotConnect) {
    *out_result = Result::Timeout;
  }
  return nullptr;
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
  // has bound its port is answered with RST. One round is a coin flip on
  // startup skew; repeating rounds inside the budget is what makes it a punch.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  Result last_result = Result::Timeout;
  Result last_logged = Result::Success;
  bool log_bind_failure = true;
  while (std::chrono::steady_clock::now() < deadline) {
    Result round_result = Result::Failure;
    const auto round_deadline =
        std::min(deadline, std::chrono::steady_clock::now() + kRoundCap);
    auto session = RaceCandidatesOnce(local, peers, round_deadline,
                                      is_initiator, log_bind_failure,
                                      &round_result);
    if (session) {
      // A simultaneous open can also complete against a socket the peer has
      // already given up on, and connect() cannot tell the two apart. The
      // handshake can: a paired session goes ready almost at once, while an
      // orphaned one never does. Confirming here keeps the retry inside the
      // punch budget, rather than handing the caller a connection that will
      // never carry anything.
      if (WaitForPairing(session, std::min(deadline,
                                           std::chrono::steady_clock::now() +
                                               kPairingConfirmation))) {
        *out_result = Result::Success;
        return session;
      }
      ZNET_LOG_DEBUG("A punched connection never paired, racing again.");
      session->Close();
      last_result = Result::Timeout;
      continue;
    }

    last_result = round_result;
    if (round_result == Result::CannotCreateSocket) {
      break;  // out of sockets; another round buys nothing
    }
    if (round_result == Result::CannotBind) {
      log_bind_failure = false;  // reported once, not once per round
    }
    // retries run for the whole budget, so per-round logging is thousands of
    // identical lines; report a reason only when it changes
    if (round_result != last_logged) {
      last_logged = round_result;
      ZNET_LOG_DEBUG("Punch round failed ({}), retrying.",
                     GetResultString(round_result));
    }
    // a refused attempt dies the moment the RST lands, so the window in which
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
