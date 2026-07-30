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
// ZDT (znet Datagram Transport). See znet/backends/zdt.h for the overview.
//

#include "znet/backends/zdt/zdt_net.h"

#include "znet/error.h"
#include "znet/logger.h"
#include "znet/util.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace znet {
namespace backends {

// ---------------------------------------------------------------------------
// UDPSocket
// ---------------------------------------------------------------------------

UDPSocket::~UDPSocket() {
  Close();
}

Result UDPSocket::Open(InetProtocolVersion ipv) {
  const SocketHandle raw = socket(GetDomainByInetProtocolVersion(ipv), SOCK_DGRAM, 0);
  if (!IsValidSocketHandle(raw)) {
    ZNET_LOG_ERROR("ZDT: failed to create UDP socket: {}", GetLastErrorInfo());
    return Result::CannotCreateSocket;
  }
  const char option = 1;
  setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
  // published last, so nothing can pick the handle up before it is configured
  socket_.store(raw, std::memory_order_release);
  return Result::Success;
}

Result UDPSocket::Bind(const InetAddress& addr) {
  if (bind(handle(), addr.handle_ptr(), addr.addr_size()) != 0) {
    ZNET_LOG_ERROR("ZDT: failed to bind UDP socket to {}: {}", addr.readable(),
                   GetLastErrorInfo());
    return Result::CannotBind;
  }
  return Result::Success;
}

bool UDPSocket::SendTo(const InetAddress& addr, const void* data, size_t len) {
#ifdef TARGET_WIN
  int n = sendto(handle(), static_cast<const char*>(data), static_cast<int>(len),
                 0, addr.handle_ptr(), addr.addr_size());
#else
  ssize_t n = sendto(handle(), static_cast<const char*>(data), len, 0,
                     addr.handle_ptr(), addr.addr_size());
#endif
  if (n < 0) {
    ZNET_LOG_DEBUG("ZDT: sendto {} failed: {}", addr.readable(),
                   GetLastErrorInfo());
    return false;
  }
  return static_cast<size_t>(n) == len;
}

RecvResult UDPSocket::RecvFrom(void* data, size_t cap, size_t& out_len,
                               std::shared_ptr<InetAddress>& out_from) {
  sockaddr_storage from{};
  socklen_t from_len = sizeof(from);
#ifdef TARGET_WIN
  int n = recvfrom(handle(), static_cast<char*>(data), static_cast<int>(cap), 0,
                   reinterpret_cast<sockaddr*>(&from), &from_len);
#else
  ssize_t n = recvfrom(handle(), static_cast<char*>(data), cap, 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
  if (n < 0) {
#ifdef TARGET_WIN
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
      return RecvResult::WouldBlock;
    }
#else
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return RecvResult::WouldBlock;
    }
#endif
    return RecvResult::Error;
  }
  out_len = static_cast<size_t>(n);
  out_from = std::shared_ptr<InetAddress>(
      InetAddress::from(reinterpret_cast<sockaddr*>(&from)));
  return RecvResult::Received;
}

bool UDPSocket::SetBlocking(bool blocking) {
  return SetSocketBlocking(handle(), blocking);
}

bool UDPSocket::SetReceiveTimeout(std::chrono::milliseconds timeout) {
#ifdef TARGET_WIN
  DWORD ms = static_cast<DWORD>(timeout.count());
  return setsockopt(handle(), SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  return setsockopt(handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UDPSocket::SetReceiveBufferSize(int bytes) {
  return setsockopt(handle(), SOL_SOCKET, SO_RCVBUF,
                    reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool UDPSocket::SetDontFragment(bool enabled) {
#if defined(TARGET_LINUX)
  int value = enabled ? IP_PMTUDISC_DO : IP_PMTUDISC_WANT;
  return setsockopt(handle(), IPPROTO_IP, IP_MTU_DISCOVER, &value,
                    sizeof(value)) == 0;
#elif defined(TARGET_APPLE)
  int value = enabled ? 1 : 0;
  return setsockopt(handle(), IPPROTO_IP, IP_DONTFRAG, &value, sizeof(value)) == 0;
#elif defined(TARGET_WIN)
  DWORD value = enabled ? 1 : 0;
  return setsockopt(handle(), IPPROTO_IP, IP_DONTFRAGMENT,
                    reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
  (void)enabled;
  return false;
#endif
}

std::shared_ptr<InetAddress> UDPSocket::local_address() {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (getsockname(handle(), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    ZNET_LOG_ERROR("ZDT: getsockname failed: {}", GetLastErrorInfo());
    return nullptr;
  }
  return std::shared_ptr<InetAddress>(
      InetAddress::from(reinterpret_cast<sockaddr*>(&ss)));
}

bool UDPSocket::Shutdown() {
  return ShutdownSocket(handle());
}

Result UDPSocket::Close() {
  // exchange rather than test-then-clear, so two threads arriving together
  // cannot both close: the second would land on a reused descriptor.
  const SocketHandle raw = socket_.exchange(kSocketInvalid, std::memory_order_acq_rel);
  if (IsValidSocketHandle(raw)) {
    CloseSocket(raw);
  }
  return Result::Success;
}

// ---------------------------------------------------------------------------
// ZDTInbox
// ---------------------------------------------------------------------------

bool ZDTInbox::Push(const uint8_t* data, size_t len, size_t limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= limit) {
    dropped_++;
    return false;
  }
  queue_.emplace_back(data, data + len);
  return true;
}

void ZDTInbox::Drain(std::deque<std::vector<uint8_t>>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out.swap(queue_);
}

size_t ZDTInbox::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

}  // namespace backends
}  // namespace znet
