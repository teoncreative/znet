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

namespace znet {
namespace p2p {

std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& peer_candidates,
    bool is_initiator, ConnectionType connection_type,
    std::chrono::milliseconds timeout, Result* out_result) {
  Result reason = Result::Failure;
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
  if (out_result != nullptr) {
    *out_result = reason;
  }
  return session;
}

}  // namespace p2p
}  // namespace znet
