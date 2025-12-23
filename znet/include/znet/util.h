
//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"
#include "znet/types.h"

#include <iomanip>
#include <ostream>
#include <sstream>

#define ZNET_BIND_FN(fn)                                    \
  [this](auto&&... args) -> decltype(auto) {                \
    return this->fn(std::forward<decltype(args)>(args)...); \
  }

#define ZNET_BIND_GLOBAL_FN(fn)                       \
  [](auto&&... args) -> decltype(auto) {              \
    return fn(std::forward<decltype(args)>(args)...); \
  }

namespace znet {

template<class...>
using void_t = void;

template <class T>
std::string ToHex(const T& numValue, int width) {
  std::ostringstream stream;
  stream << "0x"
         << std::setfill('0') << std::setw(width)
         << std::hex << +numValue;
  return stream.str();
}

std::string GeneratePeerName();

inline bool IsValidSocketHandle(SocketHandle handle) {
#ifdef TARGET_WIN
  return handle != INVALID_SOCKET;
#else
  return handle >= 0;
#endif
}

inline bool CloseSocket(SocketHandle socket) {
  if (IsValidSocketHandle(socket)) {
#ifdef TARGET_WIN
    return closesocket(socket) == 0;
#else
    return close(socket) == 0;
#endif
  }
  return false;
}

inline void SetTCPNoDelay(SocketHandle socket) {
  int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
}

inline bool SetSocketBlocking(SocketHandle socket, bool blocking) {
#ifdef TARGET_WIN
  u_long mode = blocking ? 0UL : 1UL; // 1 to enable non-blocking socket
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(socket, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |=  O_NONBLOCK;
  }
  return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

}
