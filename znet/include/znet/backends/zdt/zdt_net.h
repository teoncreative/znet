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
// The two pieces of plumbing between ZDT and the operating system: a UDP
// socket wrapper and the thread-safe queue a receive loop hands datagrams to.
// Neither knows anything about the ZDT protocol.
//

#ifndef ZNET_PARENT_ZDT_NET_H
#define ZNET_PARENT_ZDT_NET_H

#include "znet/backends/backend.h"
#include "znet/buffer.h"
#include "znet/inet_addr.h"
#include "znet/metrics.h"
#include "znet/mpsc_queue.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "znet/backends/zdt/zdt_wire.h"

namespace znet {
namespace backends {

enum class RecvResult { Received, WouldBlock, Error };

// thin owner of a UDP socket. shared by a server's per-peer transports: concurrent
// sendto() on one socket is safe.
class UDPSocket {
 public:
  UDPSocket() = default;
  ~UDPSocket();
  UDPSocket(const UDPSocket&) = delete;
  UDPSocket& operator=(const UDPSocket&) = delete;

  Result Open(InetProtocolVersion ipv);
  Result Bind(const InetAddress& addr);

  bool SendTo(const InetAddress& addr, const void* data, size_t len);
  RecvResult RecvFrom(void* data, size_t cap, size_t& out_len,
                      std::shared_ptr<InetAddress>& out_from);

  bool SetBlocking(bool blocking);
  bool SetReceiveTimeout(std::chrono::milliseconds timeout);
  // headroom for bursts that arrive between drains. Best-effort: the kernel
  // clamps to its own maximum and reports no error when it does.
  bool SetReceiveBufferSize(int bytes);
  // best-effort; the handshake MTU probe needs oversized datagrams dropped
  // rather than IP-fragmented.
  bool SetDontFragment(bool enabled);

  /**
   * @brief Wakes a blocked RecvFrom without releasing the descriptor.
   *
   * For tearing down a socket another thread may be reading. Close() returns
   * the descriptor number to the OS, where the next socket or file opened
   * anywhere in the process can reuse it while that read is still running on
   * it; the destructor closes it once every holder is gone.
   */
  bool Shutdown();

  /**
   * @brief Closes the descriptor, once, however many threads arrive together.
   */
  Result Close();

  ZNET_NODISCARD bool IsValid() const {
    return IsValidSocketHandle(handle());
  }

  ZNET_NODISCARD SocketHandle handle() const {
    return socket_.load(std::memory_order_acquire);
  }
  std::shared_ptr<InetAddress> local_address();

 private:
  std::atomic<SocketHandle> socket_{kSocketInvalid};
};

// thread-safe raw-datagram queue, shared by a producer and a consumer running on
// different threads (see ZDTTransportLayer for the threading rule).
class ZDTInbox {
 public:
  // drops and returns false once `limit` datagrams are pending, so a flooding
  // peer cannot grow this queue without bound.
  bool Push(const uint8_t* data, size_t len, size_t limit);
  void Drain(std::deque<std::vector<uint8_t>>& out);
  size_t dropped() const;

 private:
  mutable std::mutex mutex_;
  std::deque<std::vector<uint8_t>> queue_;
  size_t dropped_ = 0;
};

}  // namespace backends
}  // namespace znet

#endif  // ZNET_PARENT_ZDT_NET_H
