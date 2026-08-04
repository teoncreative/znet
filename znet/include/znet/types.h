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

#include "znet/precompiled.h"

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

#if defined(ZNET_TARGET_APPLE) || defined(ZNET_TARGET_WEB) || defined(ZNET_TARGET_LINUX)
using SocketHandle = int;
using PortNumber = in_port_t;
using IPv4Address = in_addr;
using IPv6Address = in6_addr;
constexpr SocketHandle kSocketInvalid = -1;
#elif defined(ZNET_TARGET_WIN)
using SocketHandle = SOCKET;
using PortNumber = USHORT;
using IPv4Address = IN_ADDR;
using IPv6Address = IN6_ADDR;
constexpr SocketHandle kSocketInvalid = INVALID_SOCKET;  // winsock's
#endif

}  // namespace znet

#endif  // ZNET_TYPES_H_
