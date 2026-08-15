//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_TYPES_H_
#define ZNET_TYPES_H_

#include "znet/compat.h"
#include "znet/detail/platform.h"

#include <cstdint>
#include <string>

namespace znet {

/** @brief Largest single send or receive, in bytes. */
#ifndef ZNET_MAX_BUFFER_SIZE
#define ZNET_MAX_BUFFER_SIZE 4096 //16384
#endif

// 64 bits might be an overkill here but meh
using SessionId = uint64_t;

enum class Endianness { LittleEndian, BigEndian };

// evaluated at compile-time: consteval where the language has it, and a
// constexpr returning a preprocessor-resolved constant otherwise, so the
// endianness branches in Buffer still fold away in every mode.
ZNET_CONSTEVAL Endianness GetSystemEndianness() {
  return ZNET_SYSTEM_IS_BIG_ENDIAN ? Endianness::BigEndian
                                   : Endianness::LittleEndian;
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
  NotConnected,
  IncompatibleVersion,
  ConnectionRefused,
  ServerFull,
  PeerNotFound,
  NotReady,
  QueueFull,
  InvalidArgument
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
    case Result::Timeout:
      return "Timeout";
    case Result::CannotConnect:
      return "CannotConnect";
    case Result::NotConnected:
      return "NotConnected";
    case Result::IncompatibleVersion:
      return "IncompatibleVersion";
    case Result::ConnectionRefused:
      return "ConnectionRefused";
    case Result::ServerFull:
      return "ServerFull";
    case Result::PeerNotFound:
      return "PeerNotFound";
    case Result::NotReady:
      return "NotReady";
    case Result::QueueFull:
      return "QueueFull";
    case Result::InvalidArgument:
      return "InvalidArgument";
    default:
      return "Unknown";
  }
}

enum class ConnectionType {
  TCP,
  ZDT,  // znet Datagram Transport (reliable UDP with channels)
  //ENet,
  //QUIC
};

inline std::string GetConnectionTypeString(ConnectionType type) {
  switch (type) {
    case ConnectionType::TCP:
      return "TCP";
    case ConnectionType::ZDT:
      return "ZDT";
    default:
      return "Unknown";
  }
}

// The platform's socket vocabulary, respelled so that no znet header has to
// include <winsock2.h> or <netinet/in.h> to declare anything. Each of these is
// layout-identical to the type it stands in for; inet_addr.cc static_asserts
// that against the real system headers, so a platform where the assumption
// stops holding fails to build rather than to work.
#if defined(ZNET_TARGET_WIN)
using SocketHandle = uintptr_t;  // winsock's SOCKET
using SockLen = int;
constexpr SocketHandle kSocketInvalid = ~static_cast<SocketHandle>(0);
#else
using SocketHandle = int;
using SockLen = unsigned int;  // socklen_t
constexpr SocketHandle kSocketInvalid = -1;
#endif

/** @brief A port in host byte order. */
using PortNumber = uint16_t;

/** @brief Four bytes of IPv4 address in network order, like in_addr. */
struct IPv4Address {
  uint8_t bytes[4];
};

/** @brief Sixteen bytes of IPv6 address in network order, like in6_addr. */
struct IPv6Address {
  uint8_t bytes[16];
};

/** @brief Whether a socket call returned a usable handle. */
inline bool IsValidSocketHandle(SocketHandle handle) {
#ifdef ZNET_TARGET_WIN
  return handle != kSocketInvalid;
#else
  return handle >= 0;
#endif
}

/**
 * @brief htons/ntohs for a port, resolved from the endianness compat.h already
 *        detects so that no caller needs the platform's socket headers. The
 *        conversion is its own inverse, hence the one function.
 */
constexpr uint16_t SwapPortByteOrder(uint16_t port) {
  return ZNET_SYSTEM_IS_BIG_ENDIAN
             ? port
             : static_cast<uint16_t>((port << 8) | (port >> 8));
}

}  // namespace znet

#endif  // ZNET_TYPES_H_
