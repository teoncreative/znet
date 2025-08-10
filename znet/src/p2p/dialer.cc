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

Result Punch(const std::shared_ptr<InetAddress>& local,
             const std::shared_ptr<InetAddress>& peer,
             int timeout_ms) {
  ZNET_LOG_INFO("Attempting to punch to {} from {}", peer->readable(), local->readable());
  if (!local || !peer || !local->is_valid() || !peer->is_valid()) {
    return Result::Failure;
  }

  const int domain = GetDomainByInetProtocolVersion(local->ipv());
  SocketHandle socket_handle = socket(domain, SOCK_STREAM, 0);
  if (!IsValidSocketHandle(socket_handle)) {
    return Result::CannotCreateSocket;
  }

  int option = 1;
  setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, (char*)&option, sizeof(option));

  if (local->ipv() == InetProtocolVersion::IPv6) {
    setsockopt(socket_handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&option, sizeof(option));
  }

  if (bind(socket_handle, local->handle_ptr(), local->addr_size()) != 0) {
    CloseSocket(socket_handle);
    ZNET_LOG_ERROR("Failed to bind socket to {}: {} ({})", local->readable(), LastErr(), GetLastErrorInfo());
    return Result::CannotBind;
  }

  SetSocketBlocking(socket_handle, false);

  if (connect(socket_handle, peer->handle_ptr(), peer->addr_size()) != 0 &&
      !WouldBlock(LastErr())) {
    CloseSocket(socket_handle);
    return Result::CannotConnect;
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
      return Result::Timeout;
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
      return Result::Failure;
    }

    if (result == 0) {
      continue;
    }

    if (FD_ISSET(socket_handle, &write_set) || FD_ISSET(socket_handle, &error_set)) {
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, (char*)&socket_error, &length);
      if (socket_error == 0) {
        return Result::Success;
      } else {
        CloseSocket(socket_handle);
        return Result::CannotConnect;
      }
    }
  }

  return Result::Failure;
}

}
}