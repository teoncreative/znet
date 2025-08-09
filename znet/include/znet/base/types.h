//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"

namespace znet {

// Max buffer size defines how big send and receive of each data can be.
#ifndef ZNET_MAX_BUFFER_SIZE
#define ZNET_MAX_BUFFER_SIZE 4096 //16384
#endif

// When this macro is given as a port, makes the system select a port instead
// Use the client.local_address() function to get the port on Client
// Use the server.bind_address() function to get the port on Server
#define ZNET_PORT_AUTO 0

using PacketId = uint64_t;
using SessionId = uint64_t;

enum class Endianness { LittleEndian, BigEndian };

#if defined(__cpp_lib_endian) && (__cpp_lib_endian >= 201907L)
#include <bit>

constexpr Endianness GetSystemEndianness() {
  if constexpr (std::endian::native == std::endian::big) {
    return Endianness::BigEndian;
  } else {
    return Endianness::LittleEndian;
  }
}
#else
inline Endianness GetSystemEndianness() {
  static union {
    uint32_t i;
    uint8_t c[4];
  } u = {0x01020304};
  static Endianness e = (u.c[0] == 0x01) ? Endianness::BigEndian : Endianness::LittleEndian;
  return e;
}
#endif

enum class Result {
  Success,
  Failure,
  AlreadyStopped,
  AlreadyClosed,
  AlreadyDisconnected,
  CannotBind,
  NotBound,
  InvalidAddress,
  InvalidRemoteAddress,
  CannotCreateSocket,
  CannotListen,
  AlreadyConnected,
  AlreadyListening,
  NotInitialized,
  AlreadyBound,
  InvalidBackend,
  InvalidTransport
};

inline std::string GetResultString(Result result) {
  switch (result) {
    case Result::Success:
      return "Success";
    case Result::Failure:
      return "Failure";
    case Result::AlreadyStopped:
      return "AlreadyStopped";
    case Result::AlreadyClosed:
      return "AlreadyClosed";
    case Result::AlreadyDisconnected:
      return "AlreadyDisconnected";
    case Result::NotBound:
      return "NotBound";
    case Result::CannotBind:
      return "CannotBind";
    case Result::InvalidAddress:
      return "InvalidAddress";
    case Result::InvalidRemoteAddress:
      return "InvalidRemoteAddress";
    case Result::CannotCreateSocket:
      return "CannotCreateSocket";
    case Result::CannotListen:
      return "CannotListen";
    case Result::AlreadyConnected:
      return "AlreadyConnected";
    case Result::AlreadyListening:
      return "AlreadyListening";
    case Result::NotInitialized:
      return "NotInitialized";
    case Result::AlreadyBound:
      return "AlreadyBound";
    case Result::InvalidBackend:
      return "InvalidBackend";
    case Result::InvalidTransport:
      return "InvalidTransport";
    default:
      return "Unknown";
  }
}

enum class ConnectionType {
  TCP,
  RUDP,
  //RakNet,
  //ENet,
  //WebSocket,
};

#if defined(TARGET_APPLE) || defined(TARGET_WEB) || defined(TARGET_LINUX)
using SocketHandle = int;
using PortNumber = in_port_t;
using IPv4Address = in_addr;
using IPv6Address = in6_addr;
#define INVALID_SOCKET -1
#elif defined(TARGET_WIN)
using SocketHandle = SOCKET;
using PortNumber = USHORT;
using IPv4Address = IN_ADDR;
using IPv6Address = IN6_ADDR;
#endif

}  // namespace znet