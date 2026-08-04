//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PARENT_PUNCH_H
#define ZNET_PARENT_PUNCH_H

#include "znet/inet_addr.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"

#include <vector>

namespace znet {
namespace p2p {

inline bool IsInitiator(uint64_t punch_id,
                 const std::string& self_id,
                 const std::string& peer_id) {
  bool use_smaller = ((punch_id & 1ULL) == 0ULL);

  bool self_is_smaller = (self_id < peer_id);

  if (use_smaller) {
    if (self_is_smaller) {
      return true;
    } else {
      return false;
    }
  } else {
    if (!self_is_smaller) {
      return true;
    } else {
      return false;
    }
  }
}

/**
 * @brief Punches to the first of `peer_candidates` that answers.
 *
 * Candidates are the same peer seen from different places, typically its
 * public (NAT-observed) endpoint and its private one: two peers behind the
 * same NAT usually cannot reach each other's public mapping, and the private
 * address is the one that works. ZDT races every candidate on one socket;
 * TCP cycles through them across connect attempts.
 */
std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& peer_candidates,
    Result* out_result, bool is_initiator,
    ConnectionType connection_type = ConnectionType::ZDT,
    int timeout_ms = 5000);

inline std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::shared_ptr<InetAddress>& peer,
    Result* out_result, bool is_initiator,
    ConnectionType connection_type = ConnectionType::ZDT,
    int timeout_ms = 5000) {
  return PunchSync(local,
                   std::vector<std::shared_ptr<InetAddress>>{peer},
                   out_result, is_initiator, connection_type, timeout_ms);
}

}  // namespace p2p
}  // namespace znet
#endif  //ZNET_PARENT_PUNCH_H
