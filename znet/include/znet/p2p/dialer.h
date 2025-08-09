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
// Created by Metehan Gezer on 08/08/2025.
//

#ifndef ZNET_PARENT_PUNCH_H
#define ZNET_PARENT_PUNCH_H

#include "znet/base/inet_addr.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"

namespace znet {
namespace p2p {

class Dialer {
 public:
  static Result Punch(const std::shared_ptr<InetAddress>& local,
                      const std::shared_ptr<InetAddress>& peer,
                      int timeout_ms = 5000) {
    if (!local || !peer || !local->is_valid() || !peer->is_valid()) {
      return Result::Failure;
    }

    const int domain = GetDomainByInetProtocolVersion(local->ipv());
    SocketHandle listen_socket = socket(domain, SOCK_STREAM, 0);
    SocketHandle active_socket = socket(domain, SOCK_STREAM, 0);
    if (!IsValidSocketHandle(listen_socket) ||
        !IsValidSocketHandle(active_socket)) {
      Cleanup(listen_socket, active_socket);
      return Result::CannotCreateSocket;
    }

    const char option = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &option,
               sizeof(option));
    setsockopt(active_socket, SOL_SOCKET, SO_REUSEADDR, &option,
               sizeof(option));
#ifndef TARGET_WIN
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEPORT, &option,
               sizeof(option));
    setsockopt(active_socket, SOL_SOCKET, SO_REUSEPORT, &option,
               sizeof(option));
#endif
    if (local->ipv() == InetProtocolVersion::IPv6) {
      setsockopt(listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, &option,
                 sizeof(option));
      setsockopt(active_socket, IPPROTO_IPV6, IPV6_V6ONLY, &option,
                 sizeof(option));
    }

    if (bind(listen_socket, local->handle_ptr(), local->addr_size()) != 0) {
      Cleanup(listen_socket, active_socket);
      ZNET_LOG_ERROR("Failed to bind listen socket to {}", local->readable());
      return Result::CannotBind;
    }
    if (bind(active_socket, local->handle_ptr(), local->addr_size()) != 0) {
      ZNET_LOG_ERROR("Failed to active listen socket to {}", local->readable());
      return Result::CannotBind;
    }

    SetSocketBlocking(listen_socket, false);
    SetSocketBlocking(active_socket, false);

    if (listen(listen_socket, 1) != 0) {
      Cleanup(listen_socket, active_socket);
      return Result::CannotListen;
    }

    if (connect(active_socket, peer->handle_ptr(), peer->addr_size()) != 0 &&
        !WouldBlock(LastErr())) {
      Cleanup(listen_socket, active_socket);
      return Result::CannotConnect;
    }
    std::chrono::steady_clock::duration connection_timeout =
        std::chrono::seconds(60);
    std::chrono::steady_clock::time_point start_time;
    while (true) {
      fd_set r, w;
      FD_ZERO(&r);
      FD_ZERO(&w);
      FD_SET(listen_socket, &r);
      FD_SET(active_socket, &w);  // connect completion
      struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};

      int n =
          select(std::max(listen_socket, active_socket) + 1, &r, &w, NULL, &tv);
      if (n <= 0) {
        continue;
      }

      bool have_connect = false, have_accept = false;
      SocketHandle established = INVALID_SOCKET;

      if (FD_ISSET(active_socket, &w)) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(active_socket, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        if (err == 0) {
          established = active_socket;
          have_connect = true;
        }
      }

      if (FD_ISSET(listen_socket, &r)) {
        SocketHandle sock = accept(listen_socket, NULL, NULL);
        if (IsValidSocketHandle(sock)) {
          established = sock;
          have_accept = true;
        }
      }
      if (have_accept || have_connect) {
        return Result::Success;
      }
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (connection_timeout.count() > 0 && elapsed > connection_timeout) {
        ZNET_LOG_INFO("Punch timed out");
        return Result::Timeout;
      }
    }
    return Result::Failure;
  }

 private:
  static void Cleanup(SocketHandle a, SocketHandle b) {
    CloseSocket(a);
    CloseSocket(b);
  }

  static int LastErr() {
#ifdef TARGET_WIN
    return WSAGetLastError();
#else
    return errno;
#endif
  }

  static bool WouldBlock(int e) {
#ifdef TARGET_WIN
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return e == EINPROGRESS || e == EWOULDBLOCK || e == EALREADY;
#endif
  }
};

}  // namespace p2p
}  // namespace znet
#endif  //ZNET_PARENT_PUNCH_H
