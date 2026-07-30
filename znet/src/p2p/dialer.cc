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

std::shared_ptr<PeerSession> PunchSync(const std::shared_ptr<InetAddress>& local,
                                       const std::shared_ptr<InetAddress>& peer,
                                       Result* out_result,
                                       bool is_initiator,
                                       ConnectionType connection_type,
                                       int timeout_ms) {
  if (connection_type == ConnectionType::TCP) {
    return PunchSyncTCP(local, peer, out_result, is_initiator, timeout_ms);
  }
  if (connection_type == ConnectionType::ZDT) {
    return PunchSyncZDT(local, peer, out_result, is_initiator, timeout_ms);
  }
  *out_result = Result::InvalidBackend;
  return nullptr;
}

}  // namespace p2p
}  // namespace znet
