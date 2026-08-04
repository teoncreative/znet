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

namespace znet {

/** @brief Largest single send or receive, in bytes. */
#ifndef ZNET_MAX_BUFFER_SIZE
#define ZNET_MAX_BUFFER_SIZE 4096 //16384
#endif

/**
 * @brief Passed as a port, lets the system pick one rather than binding a
 *        specific port.
 *
 * Read the chosen port back with Client::local_address() or
 * Server::bind_address().
 */
#define ZNET_PORT_AUTO 0

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
  PeerNotFound
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