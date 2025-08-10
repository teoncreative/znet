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
// Created by Metehan Gezer on 08/08/2025.
//

#ifndef ZNET_PARENT_PUNCH_H
#define ZNET_PARENT_PUNCH_H

#include "znet/base/inet_addr.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/error.h"

namespace znet {
namespace p2p {

std::shared_ptr<PeerSession> Punch(const std::shared_ptr<InetAddress>& local,
                                   const std::shared_ptr<InetAddress>& peer,
                                   Result* out_result,
                                   int timeout_ms = 5000);

}  // namespace p2p
}  // namespace znet
#endif  //ZNET_PARENT_PUNCH_H
