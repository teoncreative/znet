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
// Created by Metehan Gezer on 10/08/2025.
//

#include "znet/p2p/dialer.h"

#include "znet/backends/tcp.h"
#include "znet/error.h"
#include "znet/transport.h"

namespace znet {
namespace p2p {

int LastErr() {
#ifdef TARGET_WIN
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool WouldBlock(int e) {
#ifdef TARGET_WIN
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
  return e == EINPROGRESS || e == EWOULDBLOCK || e == EALREADY;
#endif
}

std::shared_ptr<PeerSession> PunchSyncTCP(const std::shared_ptr<InetAddress>& local,
                                       const std::shared_ptr<InetAddress>& peer,
                                       Result* out_result,
                                       bool is_initiator,
                                       int timeout_ms) {
  ZNET_LOG_INFO("Attempting to punch to {} from {}", peer->readable(), local->readable());
  if (!local || !peer || !local->is_valid() || !peer->is_valid()) {
    *out_result = Result::InvalidAddress;
    return nullptr;
  }

  const int domain = GetDomainByInetProtocolVersion(local->ipv());
  SocketHandle socket_handle = socket(domain, SOCK_STREAM, 0);
  if (!IsValidSocketHandle(socket_handle)) {
    *out_result = Result::CannotCreateSocket;
    return nullptr;
  }

  int option = 1;
  setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, (char*)&option, sizeof(option));

  if (local->ipv() == InetProtocolVersion::IPv6) {
    setsockopt(socket_handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&option, sizeof(option));
  }

  if (bind(socket_handle, local->handle_ptr(), local->addr_size()) != 0) {
    CloseSocket(socket_handle);
    ZNET_LOG_ERROR("Failed to bind socket to {}: {} ({})", local->readable(), LastErr(), GetLastErrorInfo());
    *out_result = Result::CannotBind;
    return nullptr;
  }

  SetSocketBlocking(socket_handle, false);
  SetTCPNoDelay(socket_handle);

  if (connect(socket_handle, peer->handle_ptr(), peer->addr_size()) != 0 &&
      !WouldBlock(LastErr())) {
    CloseSocket(socket_handle);
    *out_result = Result::CannotConnect;
    return nullptr;
  }

  sockaddr_storage local_ss{};
  socklen_t local_len = sizeof(local_ss);
  if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&local_ss), &local_len) == 0) {
    auto confirm_address = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
    if (confirm_address) {
      ZNET_LOG_DEBUG("getsockname: {}", confirm_address->readable());
    } else {
      ZNET_LOG_DEBUG("getsockname: invalid address");
    }
  } else {
    ZNET_LOG_ERROR("getsockname failed: {}", GetLastErrorInfo());
  }

  std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration connection_timeout = std::chrono::milliseconds(timeout_ms);

  while (true) {
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    if (elapsed > connection_timeout) {
      CloseSocket(socket_handle);
      *out_result = Result::Timeout;
      return nullptr;
    }

    fd_set write_set;
    fd_set error_set;
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    FD_SET(socket_handle, &write_set);
    FD_SET(socket_handle, &error_set);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200ms polling interval

    int result = select(socket_handle + 1, NULL, &write_set, &error_set, &tv);
    if (result < 0) {
#ifdef TARGET_WIN
      if (LastErr() == WSAEINTR) {
        continue;
      }
#else
      if (errno == EINTR) {
        continue;
      }
#endif
      CloseSocket(socket_handle);
      *out_result = Result::Failure;
      return nullptr;
    }

    if (result == 0) {
      continue;
    }

    if (FD_ISSET(socket_handle, &write_set) || FD_ISSET(socket_handle, &error_set)) {
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, (char*)&socket_error, &length);
      if (socket_error == 0) {
        SetSocketBlocking(socket_handle, true);
        *out_result = Result::Success;
        return std::make_shared<PeerSession>(local, peer,
                                             std::make_unique<backends::TCPTransportLayer>(socket_handle),
                                             ConnectionType::TCP,
                                             is_initiator,
                                             true);
      }
      CloseSocket(socket_handle);
      *out_result = Result::CannotConnect;
      return nullptr;
    }
  }
}

std::shared_ptr<PeerSession> PunchSync(const std::shared_ptr<InetAddress>& local,
                                       const std::shared_ptr<InetAddress>& peer,
                                       Result* out_result,
                                       bool is_initiator,
                                       ConnectionType connection_type,
                                       int timeout_ms) {
  if (connection_type == ConnectionType::TCP) {
    return PunchSyncTCP(local, peer, out_result, is_initiator, timeout_ms);
  }
  return nullptr;
}

}
}