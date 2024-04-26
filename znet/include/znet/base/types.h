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

#ifndef __cpp_lib_endian
namespace std {
enum class endian {
  little = 0xDEAD,
  big = 0xFACE,
#if defined(_LIBCPP_LITTLE_ENDIAN)
  native = little
#elif defined(_LIBCPP_BIG_ENDIAN)
  native = big
#else
  native = 0xCAFE
#endif
};
}  // namespace std
#endif

namespace znet {

// Max buffer size defines how big send and receive of each data can be.
#ifndef ZNET_MAX_BUFFER_SIZE
#define ZNET_MAX_BUFFER_SIZE 4096 //16384
#endif

template <typename T>
using Weak = std::weak_ptr<T>;

template <typename T>
using Scope = std::unique_ptr<T>;

template <typename T, typename... Args>
constexpr Scope<T> CreateScope(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
using Ref = std::shared_ptr<T>;

template <typename T, typename... Args>
constexpr Ref<T> CreateRef(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

using PacketId = uint64_t;

enum class Endianness { LittleEndian, BigEndian };

constexpr Endianness GetSystemEndianness() {
  if constexpr (std::endian::native == std::endian::big) {
    return Endianness::BigEndian;
  } else if constexpr (std::endian::native == std::endian::little) {
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
  InvalidAddress,
  InvalidRemoteAddress,
  CannotCreateSocket,
  CannotListen,
  AlreadyConnected,
  AlreadyListening
};


}  // namespace znet