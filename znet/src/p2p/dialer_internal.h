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
// Per-backend hole-punchers. PunchSync (dialer.cc) picks one by ConnectionType;
// each lives in its own translation unit. Internal to the library.
//

#ifndef ZNET_PARENT_DIALER_INTERNAL_H
#define ZNET_PARENT_DIALER_INTERNAL_H

#include "znet/inet_addr.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"

namespace znet {
namespace p2p {

// Each returns a connected, self-managed PeerSession and sets *out_result to
// Result::Success, or returns nullptr with the failure reason in *out_result.
std::shared_ptr<PeerSession> PunchSyncTCP(
    const std::shared_ptr<InetAddress>& local,
    const std::shared_ptr<InetAddress>& peer, Result* out_result,
    bool is_initiator, int timeout_ms);

std::shared_ptr<PeerSession> PunchSyncZDT(
    const std::shared_ptr<InetAddress>& local,
    const std::shared_ptr<InetAddress>& peer, Result* out_result,
    bool is_initiator, int timeout_ms);

}  // namespace p2p
}  // namespace znet

#endif  // ZNET_PARENT_DIALER_INTERNAL_H
