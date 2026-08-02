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
// Shared setup for the znet-linked benchmarks (znet-bench, fanout-bench).
// The library-agnostic headers in common/ must not include znet headers, so
// this lives apart.
//

#ifndef ZNET_BENCH_ZNET_TUNING_H
#define ZNET_BENCH_ZNET_TUNING_H

#include "znet/options.h"
#include "znet/types.h"

namespace bench {

// Binds an ephemeral port, notes what the OS picked, releases it. Another
// process can grab it before rebinding, so callers retry on connect failure.
inline znet::PortNumber FreePort() {
#ifdef _WIN32
  SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
#else
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  znet::PortNumber port = ntohs(addr.sin_port);
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
  return port;
}

// Lifts queue bounds clear of the workload so the table measures protocol cost
// rather than buffer sizing. These are anti-flood caps, not tuning; the
// congestion window and max_datagrams_in_flight are left alone deliberately.
inline void ApplyBenchQueueBounds(znet::SessionOptions& options) {
  options.common.send_queue_capacity = 65536;
  options.zdt.outbound_queue_capacity = 65536;
  options.zdt.max_inbox_datagrams = 65536;
  options.zdt.max_reassemblies = 8192;
  // same 16 MB ask as the other libraries; the kernel clamps to
  // net.core.rmem_max/wmem_max, so ZNET_LOG_DEBUG shows what was granted
  options.zdt.socket_recv_buffer = 16 * 1024 * 1024;
  options.zdt.socket_send_buffer = 16 * 1024 * 1024;
}

}  // namespace bench

#endif  // ZNET_BENCH_ZNET_TUNING_H
