//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_P2P_DIALER_H_
#define ZNET_P2P_DIALER_H_

#include "znet/inet_addr.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"

#include <vector>

namespace znet {
namespace p2p {

/**
 * @brief The tiebreak both peers compute from the rendezvous-issued punch id:
 *        exactly one of them comes out the initiator, which is the side that
 *        accepts (and so decides encryption) on the punched session.
 */
inline bool IsInitiator(uint64_t punch_id, const std::string& self_id,
                        const std::string& peer_id) {
  const bool use_smaller = (punch_id & 1ULL) == 0ULL;
  const bool self_is_smaller = self_id < peer_id;
  return use_smaller == self_is_smaller;
}

/**
 * @brief Punches to the first of `peer_candidates` that answers.
 *
 * Candidates are the same peer seen from different places, typically its
 * public (NAT-observed) endpoint and its private one: two peers behind the
 * same NAT usually cannot reach each other's public mapping, and the private
 * address is the one that works. ZDT races every candidate on one socket;
 * TCP cycles through them across connect attempts.
 *
 * @param out_result the failure reason when null comes back; pass nothing if
 *        the session alone is enough.
 */
std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::vector<std::shared_ptr<InetAddress>>& peer_candidates,
    bool is_initiator, ConnectionType connection_type = ConnectionType::ZDT,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
    Result* out_result = nullptr);

inline std::shared_ptr<PeerSession> PunchSync(
    const std::shared_ptr<InetAddress>& local,
    const std::shared_ptr<InetAddress>& peer, bool is_initiator,
    ConnectionType connection_type = ConnectionType::ZDT,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
    Result* out_result = nullptr) {
  return PunchSync(local, std::vector<std::shared_ptr<InetAddress>>{peer},
                   is_initiator, connection_type, timeout, out_result);
}

}  // namespace p2p
}  // namespace znet

#endif  // ZNET_P2P_DIALER_H_
