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
// ZDT (znet Datagram Transport). A self-contained reliable-UDP transport with
// channels (reliable/unreliable x ordered/unordered), a RakNet-style versioned
// and spoof-resistant handshake, fragmentation and keepalive. It slots in below
// the existing send/recv pipeline as a TransportLayer, so encryption, compression
// and serialization are unchanged. See docs/zdt-design.md for the full spec.
//

#ifndef ZNET_PARENT_ZDT_H
#define ZNET_PARENT_ZDT_H

#include "znet/backends/zdt/zdt_backends.h"
#include "znet/backends/zdt/zdt_net.h"
#include "znet/backends/zdt/zdt_transport.h"
#include "znet/backends/zdt/zdt_wire.h"

#endif  // ZNET_PARENT_ZDT_H
