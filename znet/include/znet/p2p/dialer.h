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
#include "znet/precompiled.h"
#include "znet/peer_session.h"

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
    SocketHandle listen_socket = ::socket(domain, SOCK_STREAM, 0);
    SocketHandle active_socket = ::socket(domain, SOCK_STREAM, 0);
    if (!IsValidSocketHandle(listen_socket) || !IsValidSocketHandle(active_socket)) {
      Cleanup(listen_socket, active_socket);
      return Result::Failure;
    }

    const char option = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &option,sizeof(option));
    setsockopt(active_socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
#ifndef TARGET_WIN
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
    setsockopt(active_socket, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
#endif
    if (local->ipv() == InetProtocolVersion::IPv6) {
      setsockopt(listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, &option, sizeof(option));
      setsockopt(active_socket, IPPROTO_IPV6, IPV6_V6ONLY, &option, sizeof(option));
    }

    if (bind(listen_socket, local->handle_ptr(), local->addr_size()) != 0 ||
        bind(active_socket, local->handle_ptr(), local->addr_size()) != 0) {
      Cleanup(listen_socket, active_socket);
      return Result::Failure;
    }

    SetSocketBlocking(listen_socket, false);
    SetSocketBlocking(active_socket, false);

    if (::listen(listen_socket, 1) != 0) {
      Cleanup(listen_socket, active_socket);
      return Result::Failure;
    }

    if (connect(active_socket, peer->handle_ptr(), peer->addr_size()) != 0 &&
        !WouldBlock(LastErr())) {
      Cleanup(listen_socket, active_socket);
      return Result::Failure;
    }

    bool via_accept;
    if (!Race(listen_socket, active_socket, timeout_ms, &via_accept)) {
      Cleanup(listen_socket, active_socket);
      return Result::Failure;
    }

    if (via_accept) {
      CloseSocket(active_socket);
    } else {
      CloseSocket(listen_socket);
    }
    return Result::Success;
  }

 private:
  static bool Race(SocketHandle listen_socket, SocketHandle active_socket, int timeout_ms,
                   bool* out_via_accept) {
    fd_set rd, wr, er;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&er);
    FD_SET(listen_socket, &rd);
    FD_SET(active_socket, &wr);
    FD_SET(active_socket, &er);

#ifdef TARGET_WIN
    TIMEVAL tv{static_cast<LONG>(timeout_ms / 1000),
               static_cast<LONG>((timeout_ms % 1000) * 1000)};
    int nf = select(0, &rd, &wr, &er, timeout_ms >= 0 ? &tv : nullptr);
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int maxfd = static_cast<int>(std::max(listen_socket, active_socket));
    int nf = select(maxfd + 1, &rd, &wr, &er,
                      timeout_ms >= 0 ? &tv : nullptr);
#endif
    if (nf <= 0) {
      return false;
    }

    if (FD_ISSET(listen_socket, &rd)) {
      sockaddr_storage remote_ss{};
      socklen_t remote_len = sizeof(remote_ss);
      SocketHandle a = accept(listen_socket,
                                reinterpret_cast<sockaddr*>(&remote_ss),
                                &remote_len);
      if (!IsValidSocketHandle(a)) {
        return false;
      }

      SetSocketBlocking(a, false);

      sockaddr_storage local_ss{};
      socklen_t local_len = sizeof(local_ss);
      getsockname(a, reinterpret_cast<sockaddr*>(&local_ss), &local_len);

      auto local_addr = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
      auto remote_addr = InetAddress::from(reinterpret_cast<sockaddr*>(&remote_ss));
      if (!local_addr || !remote_addr) {
        CloseSocket(a);
        return false;
      }

      /*out->session = std::make_shared<PeerSession>(
          local_addr, remote_addr, std::make_unique<TCPTransportLayer>(a));*/
      *out_via_accept = true;
      return true;
    }

    if (FD_ISSET(active_socket, &wr) || FD_ISSET(active_socket, &er)) {
      int err = 0;
      socklen_t len = sizeof(err);
      ::getsockopt(active_socket, SOL_SOCKET, SO_ERROR,
#ifdef TARGET_WIN
                   reinterpret_cast<char*>(&err),
#else
                   &err,
#endif
                   &len);
      if (err != 0) {
        return false;
      }

      sockaddr_storage local_ss{};
      socklen_t local_len = sizeof(local_ss);
      ::getsockname(active_socket, reinterpret_cast<sockaddr*>(&local_ss),
                    &local_len);
      auto local_addr = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
      if (!local_addr) {
        return false;
      }

      /*out->session = std::make_shared<PeerSession>(
          local_addr, peer,
          std::make_unique<TCPTransportLayer>(active_socket),
          true)*/;
      *out_via_accept = false;
      return true;
    }
    return false;
  }

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

}
}
#endif  //ZNET_PARENT_PUNCH_H
