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
#include <bit>

namespace znet {

// Max buffer size defines how big send and receive of each data can be.
#ifndef ZNET_MAX_BUFFER_SIZE
#define ZNET_MAX_BUFFER_SIZE 4096 //16384
#endif

// When this macro is given as a port, makes the system select a port instead
// Use the client.local_address() function to get the port on Client
// Use the server.bind_address() function to get the port on Server
#define ZNET_PORT_AUTO 0

// 64 bits might be an overkill here but meh
using SessionId = uint64_t;

enum class Endianness { LittleEndian, BigEndian };

constexpr auto operator<=>(Endianness lhs, Endianness rhs) noexcept {
  return static_cast<int>(lhs) <=> static_cast<int>(rhs);
}

constexpr bool operator==(Endianness lhs, Endianness rhs) noexcept {
  return static_cast<int>(lhs) == static_cast<int>(rhs);
}

// must be evaluated at compile-time
consteval Endianness GetSystemEndianness() {
  if constexpr (std::endian::native == std::endian::big) {
    return Endianness::BigEndian;
  } else {
    return Endianness::LittleEndian;
  }
}

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
  InvalidTransport,
  Timeout,
  CannotConnect,
  NotConnected
};

constexpr auto operator<=>(Result lhs, Result rhs) noexcept {
  return static_cast<int>(lhs) <=> static_cast<int>(rhs);
}

constexpr bool operator==(Result lhs, Result rhs) noexcept {
  return static_cast<int>(lhs) == static_cast<int>(rhs);
}

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
    case Result::Timeout:
      return "Timeout";
    case Result::CannotConnect:
      return "CannotConnect";
    case Result::NotConnected:
      return "NotConnected";
    default:
      return "Unknown";
  }
}

enum class ConnectionType {
  TCP,
  //RUDP,
  //ENet,
  //QUIC
};

constexpr auto operator<=>(ConnectionType lhs, ConnectionType rhs) noexcept {
  return static_cast<int>(lhs) <=> static_cast<int>(rhs);
}

constexpr bool operator==(ConnectionType lhs, ConnectionType rhs) noexcept {
  return static_cast<int>(lhs) == static_cast<int>(rhs);
}

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
constexpr SocketHandle kSocketInvalid = INVALID_SOCKET;

}  // namespace znet