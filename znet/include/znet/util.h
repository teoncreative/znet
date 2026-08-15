
//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_UTIL_H_
#define ZNET_UTIL_H_

#include "znet/types.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

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

/**
 * @brief The calling process's id.
 *
 * For telling two processes on one machine apart: a log file name, a unix
 * socket path, a peer label. uint32_t because that is what both platforms
 * report, and it is not a handle to anything, so nothing can be done with it
 * beyond printing it.
 */
uint32_t GetProcessId();

}  // namespace znet

#endif  // ZNET_UTIL_H_
