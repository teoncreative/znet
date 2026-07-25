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

/** @brief Counters meaningful on every transport. */
struct CommonMetrics {
  uint64_t messages_sent = 0;
  uint64_t messages_received = 0;
  uint64_t message_bytes_sent = 0;  /**< After encode, before transport framing. */
  uint64_t message_bytes_received = 0;
  uint64_t send_failures = 0;  /**< Send() refused, e.g. queue full. */
  uint64_t wire_bytes_sent = 0;  /**< Including transport framing. */
  uint64_t wire_bytes_received = 0;
  uint32_t outbound_queued = 0;  /**< Sampled, not accumulated. */
};

/** @brief Counters only a TCP session reports. */
struct TCPSessionMetrics {
  uint64_t writes = 0;  /**< Send() calls that succeeded. */
  uint64_t reads = 0;  /**< Recv() calls that returned data. */
};

/** @brief Counters only a ZDT session reports. */
struct ZDTSessionMetrics {
  uint64_t datagrams_sent = 0;
  uint64_t datagrams_received = 0;
  uint64_t retransmits = 0;
  uint64_t naks_sent = 0;  /**< Gaps we reported to the peer. */
  uint64_t naks_received = 0;  /**< Gaps the peer reported to us. */
  uint64_t duplicates_dropped = 0;  /**< Deduped by the receiver. */
  uint64_t inbound_dropped = 0;  /**< Inbox full. */
  uint64_t reassemblies_dropped = 0;  /**< Incomplete, timed out or over cap. */
  /** @brief Smoothed round-trip estimate. Sampled, not accumulated. */
  uint32_t srtt_us = 0;
  /** @brief Current retransmit timeout. Sampled, not accumulated. */
  uint32_t rto_us = 0;
  uint32_t in_flight = 0;  /**< Unacked reliable datagrams. */
  /** @brief MTU the handshake settled on. Sampled, not accumulated. */
  uint32_t mtu = 0;
};

/**
 * @brief Everything a session reports. Read `transport` to know which of the
 * per-transport groups carries meaningful values.
 */
struct SessionMetrics {
  ConnectionType transport = ConnectionType::TCP;  /**< Which group below is live. */
  CommonMetrics common;
  TCPSessionMetrics tcp;
  ZDTSessionMetrics zdt;
};

/**
 * @brief Listener-scope ZDT counters.
 *
 * ZDT rejects connections before a session exists, so these cannot live on a
 * session the way the others do.
 */
struct ZDTServerMetrics {
  uint64_t handshakes_started = 0;  /**< First contact from a new address. */
  uint64_t handshakes_rejected = 0;  /**< Version mismatch, server full, banned. */
  uint64_t cookies_rejected = 0;  /**< Failed return-routability check. */
  uint64_t rate_limited = 0;  /**< Per-source handshake cap hit. */
  uint64_t datagrams_unroutable = 0;  /**< Online datagram from an unknown peer. */
};

/** @brief Listener-scope counters, across every session it accepted. */
struct ServerMetrics {
  ConnectionType transport = ConnectionType::TCP;
  uint64_t connections_accepted = 0;
  uint64_t connections_active = 0;
  ZDTServerMetrics zdt;
};

}  // namespace znet

#endif  // ZNET_PARENT_METRICS_H
