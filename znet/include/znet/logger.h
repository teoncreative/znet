
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
#include <iostream>

// pick a formatting backend. std::format is preferred wherever it exists; the
// bundled shim keeps the same `{}` call syntax on C++14/17 without pulling in
// a dependency. set ZNET_USE_FMTLIB=1 (and link fmt::fmt) to get full format
// spec support below C++20.
//
// <format> is probed rather than assumed from the language level: libc++
// shipped the C++20 language features long before the library ones, so an
// -std=c++20 build on an older Clang/Apple toolchain has no std::format at
// all. __cpp_lib_format comes from <version>.
#if ZNET_HAS_CXX20 && defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#if defined(ZNET_USE_FMTLIB) && ZNET_USE_FMTLIB
#include <fmt/format.h>
#define ZNET_FORMAT(...) ::fmt::format(__VA_ARGS__)
#elif ZNET_HAS_CXX20 && defined(__cpp_lib_format)
#include <format>
#define ZNET_FORMAT(...) ::std::format(__VA_ARGS__)
#else
#include "znet/detail/format.h"
#define ZNET_FORMAT(...) ::znet::detail::Format(__VA_ARGS__)
#endif

// https://github.com/TheCherno/Hazel/blob/e4b0493999206bd2c3ff9d30fa333bcf81f313c8/Hazel/src/Hazel/Debug/Instrumentor.h#L207
// Resolve which function signature macro will be used. Note that this only
// is resolved when the (pre)compiler starts, so the syntax highlighting
// could mark the wrong one in your editor!
#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || \
    (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)
#define ZNET_FUNC_SIGN __PRETTY_FUNCTION__
#elif defined(__DMC__) && (__DMC__ >= 0x810)
#define ZNET_FUNC_SIGN __PRETTY_FUNCTION__
#elif (defined(__FUNCSIG__) || (_MSC_VER))
#define ZNET_FUNC_SIGN __FUNCSIG__
#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || \
    (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
#define ZNET_FUNC_SIGN __FUNCTION__
#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
#define ZNET_FUNC_SIGN __FUNC__
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
#define ZNET_FUNC_SIGN __func__
#elif defined(__cplusplus) && (__cplusplus >= 201103)
#define ZNET_FUNC_SIGN __func__
#else
#define ZNET_FUNC_SIGN "Unknown"
#endif

#define ZNET_LOG_LEVEL_DEBUG 0
#define ZNET_LOG_LEVEL_INFO 1
#define ZNET_LOG_LEVEL_WARN 2
#define ZNET_LOG_LEVEL_ERROR 3
#define ZNET_LOG_LEVEL_NONE 4

#ifndef ZNET_LOG_LEVEL
#define ZNET_LOG_LEVEL ZNET_LOG_LEVEL_DEBUG
#endif

// the message is folded into __VA_ARGS__ rather than named separately, which
// is what removes the need for __VA_OPT__ (C++20) to elide the comma when a
// log call passes no arguments beyond the message.
#define ZNET_PRINTFN(fmsg, func, ...) \
  std::cout << ZNET_FORMAT(fmsg, func, ZNET_FORMAT(__VA_ARGS__)) << std::flush

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_DEBUG
#define ZNET_LOG_DEBUG(...)                                             \
ZNET_PRINTFN("\x1b[44m[debug]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n",   \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_DEBUG(...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_INFO
#define ZNET_LOG_INFO(...)                                              \
ZNET_PRINTFN("\x1b[42m[info ]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n",   \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_INFO(...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_WARN
#define ZNET_LOG_WARN(...)                                              \
ZNET_PRINTFN("\x1b[41m[warn ]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n",  \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_WARN(...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_ERROR
#define ZNET_LOG_ERROR(...)                                             \
ZNET_PRINTFN("\x1b[41m[error]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n",  \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_ERROR(...)
#endif

class LoggerInitializer {
 public:
  static bool s_Initialized;

  LoggerInitializer();
};

static LoggerInitializer s_LoggerInitializer;