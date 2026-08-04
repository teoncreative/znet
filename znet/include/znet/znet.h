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
// The umbrella: everything a typical client or server application needs.
// P2P lives behind znet/p2p.h, kept separate because most applications never
// touch it.
//

#ifndef ZNET_ZNET_H_
#define ZNET_ZNET_H_

#include "znet/buffer.h"
#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/error.h"
#include "znet/event.h"
#include "znet/init.h"
#include "znet/inet_addr.h"
#include "znet/logger.h"
#include "znet/metrics.h"
#include "znet/options.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/types.h"

#endif  // ZNET_ZNET_H_
