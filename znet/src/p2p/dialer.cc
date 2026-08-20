//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/dialer.h"

#include "dialer_internal.h"

#include "znet/logger.h"

#include <thread>

namespace znet {
namespace p2p {

namespace {

// A punched session still has to run the encryption handshake, and until it
// does its codec and handler belong to that handshake. Handing one back early
// means a caller that installs its own in the connect event replaces the
// handshake's and stalls the connection, so the punch is not finished until
// the session reports ready.
bool WaitUntilReady(const std::shared_ptr<PeerSession>& session,
                    std::chrono::steady_clock::time_point deadline) {
  while (!session->IsReady()) {
    if (!session->IsAlive() || std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& peer_candidates,
    bool is_initiator, ConnectionType connection_type,
    std::chrono::milliseconds timeout, Result* out_result) {
  Result reason = Result::Failure;
  // one budget for the punch and the handshake together, so the caller's
  // timeout still means what it says
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const int timeout_ms = static_cast<int>(timeout.count());
  std::shared_ptr<PeerSession> session;
  if (connection_type == ConnectionType::TCP) {
    session = PunchSyncTCP(local, peer_candidates, &reason, is_initiator,
                           timeout_ms);
  } else if (connection_type == ConnectionType::ZDT) {
    session = PunchSyncZDT(local, peer_candidates, &reason, is_initiator,
                           timeout_ms);
  } else {
    reason = Result::InvalidBackend;
  }
  // the TCP punch confirms pairing inside its retry loop, so this is usually
  // already true by the time it returns; ZDT settles here
  if (session && !WaitUntilReady(session, deadline)) {
    ZNET_LOG_WARN("Punched session to {} never finished its handshake",
                  session->remote_address()->readable());
    session->Close();
    session = nullptr;
    reason = Result::Timeout;
  }
  if (out_result != nullptr) {
    *out_result = reason;
  }
  return session;
}

}  // namespace p2p
}  // namespace znet
