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
// Counters for a session and for a server, scoped the same way options are:
// what every transport has lives in `common`, and each transport gets its own
// group using its own vocabulary. `transport` says which group is populated;
// the others stay zeroed, so reading the wrong one is harmless rather than
// undefined the way a union would be.
//
//   SessionMetrics m = session->metrics();
//   m.common.messages_sent;     // any transport
//   m.zdt.retransmits;          // ZDT only
//
// Collection is pull-based: the hot path only bumps plain members owned by the
// thread that already owns the object, and a snapshot is taken on demand. No
// atomics, no locks, no per-packet callbacks. Build with ZNET_ENABLE_METRICS=0
// to remove them entirely.
//

#ifndef ZNET_PARENT_METRICS_H
#define ZNET_PARENT_METRICS_H

#include "znet/precompiled.h"
#include "znet/types.h"

#include <cstdint>

#ifndef ZNET_ENABLE_METRICS
#define ZNET_ENABLE_METRICS 1
#endif

#if ZNET_ENABLE_METRICS
#define ZNET_METRIC(expr) (expr)
#else
#define ZNET_METRIC(expr) ((void)0)
#endif

namespace znet {

// Meaningful on every transport.
struct CommonMetrics {
  uint64_t messages_sent = 0;
  uint64_t messages_received = 0;
  uint64_t message_bytes_sent = 0;      // after encode, before transport framing
  uint64_t message_bytes_received = 0;
  uint64_t send_failures = 0;           // Send() refused, e.g. queue full
  uint64_t wire_bytes_sent = 0;         // including transport framing
  uint64_t wire_bytes_received = 0;
  uint32_t outbound_queued = 0;         // sampled, not accumulated
};

struct TCPSessionMetrics {
  uint64_t writes = 0;  // send() calls that succeeded
  uint64_t reads = 0;   // recv() calls that returned data
};

struct ZDTSessionMetrics {
  uint64_t datagrams_sent = 0;
  uint64_t datagrams_received = 0;
  uint64_t retransmits = 0;
  uint64_t duplicates_dropped = 0;      // deduped by the receiver
  uint64_t inbound_dropped = 0;         // inbox full
  uint64_t reassemblies_dropped = 0;    // incomplete, timed out or over cap
  // live state, sampled rather than accumulated
  uint32_t srtt_us = 0;
  uint32_t rto_us = 0;
  uint32_t in_flight = 0;               // unacked reliable datagrams
  uint32_t mtu = 0;
};

struct SessionMetrics {
  ConnectionType transport = ConnectionType::TCP;  // which group below is live
  CommonMetrics common;
  TCPSessionMetrics tcp;
  ZDTSessionMetrics zdt;
};

// ZDT rejects connections before a session exists, so these are listener-scope.
struct ZDTServerMetrics {
  uint64_t handshakes_started = 0;    // first contact from a new address
  uint64_t handshakes_rejected = 0;   // version mismatch, server full, banned
  uint64_t cookies_rejected = 0;      // failed return-routability check
  uint64_t rate_limited = 0;          // per-source handshake cap hit
  uint64_t datagrams_unroutable = 0;  // online datagram from an unknown peer
};

struct ServerMetrics {
  ConnectionType transport = ConnectionType::TCP;
  uint64_t connections_accepted = 0;
  uint64_t connections_active = 0;
  ZDTServerMetrics zdt;
};

}  // namespace znet

#endif  // ZNET_PARENT_METRICS_H
