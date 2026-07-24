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
// Connection options, scoped like Netty's: `options` configure the thing you
// created (a listener, or a client), `child_options` configure each session that
// listener accepts. A client has no children, so its `options` are the session's.
//
//   ServerConfig config{"0.0.0.0", 25000, std::chrono::seconds(10)};
//   config.options.max_connections = 4096;   // listener
//   config.child_options.tcp.no_delay = true;
//   config.child_options.zdt.cwnd = 128;     // every accepted session
//
// Options are plain structs rather than a typed-key map: every option is known
// at compile time here, so a map would buy nothing and cost a lookup. Unset
// fields simply keep the defaults below.
//

#ifndef ZNET_PARENT_OPTIONS_H
#define ZNET_PARENT_OPTIONS_H

#include "znet/precompiled.h"
#include "znet/types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>

namespace znet {

// Applies to any session regardless of transport.
struct CommonOptions {
  // Drop a session that has neither sent nor received for this long. Zero
  // disables. TCP relies on the OS unless a transport implements it.
  std::chrono::milliseconds idle_timeout{10000};
  bool collect_metrics = true;  // ignored when built with ZNET_ENABLE_METRICS=0
};

struct TCPOptions {
  bool no_delay = true;       // TCP_NODELAY
  bool reuse_address = true;
  int send_buffer_size = 0;   // 0 keeps the OS default
  int receive_buffer_size = 0;
};

// Candidate MTUs, probed in order. Fixed capacity and stored inline: a session
// copies its options, so a std::vector here would mean a heap allocation per
// connection to hold a handful of bytes.
struct MTULadder {
  static constexpr size_t kCapacity = 4;
  std::array<uint16_t, kCapacity> rungs{1492, 1200, 576, 0};
  uint8_t count = 3;

  constexpr void Set(std::initializer_list<uint16_t> values) {
    count = 0;
    for (uint16_t v : values) {
      if (count == kCapacity) {
        break;
      }
      rungs[count++] = v;
    }
  }
  constexpr const uint16_t* begin() const { return rungs.data(); }
  constexpr const uint16_t* end() const { return rungs.data() + count; }
  constexpr uint16_t front() const { return rungs[0]; }
  constexpr uint16_t back() const { return rungs[count ? count - 1u : 0u]; }
  constexpr bool empty() const { return count == 0; }
};

// ZDT tunables. See docs/zdt-design.md.
struct ZDTOptions {
  MTULadder mtu_ladder;  // probed descending
  std::chrono::milliseconds rto_min{100};
  std::chrono::milliseconds rto_max{2000};
  int max_retries = 10;
  std::chrono::milliseconds keepalive_interval{1000};
  std::chrono::milliseconds idle_timeout{10000};
  int cwnd = 64;
  std::chrono::milliseconds handshake_retransmit{250};
  int handshake_retries_per_rung = 4;
  std::chrono::seconds cookie_secret_rotation{120};
  int max_half_open = 1024;
  int max_connections = 4096;
  int per_source_handshake_rate = 20;  // max handshake msgs / source / second
  std::chrono::milliseconds reassembly_timeout{5000};  // drop partial messages
  // queue caps, bounding a flooding peer and an application that outruns the link
  size_t max_inbox_datagrams = 4096;    // pending raw datagrams per connection
  size_t max_outbound_messages = 4096;  // queued application messages
  size_t max_reassemblies = 256;        // concurrent partial messages
};

// Per-session options; a server applies these to every session it accepts.
struct SessionOptions {
  CommonOptions common;
  TCPOptions tcp;
  ZDTOptions zdt;
};

// Listener-scope options, i.e. things that exist before any session does.
struct ServerOptions {
  int backlog = 0;           // 0 uses SOMAXCONN (TCP only)
  int max_connections = 0;   // 0 means unlimited
  bool reuse_address = true;
};

}  // namespace znet

#endif  // ZNET_PARENT_OPTIONS_H
