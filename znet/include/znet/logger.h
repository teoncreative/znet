
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

#include <fmt/core.h>

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

#ifdef _MSC_VER
#define ZNET_PRINTFN(fmsg, func, msg, ...)               \
  fmt::print(fmsg, func, fmt::format(msg, __VA_ARGS__)); \
  std::cout << std::flush

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_DEBUG
#define ZNET_LOG_DEBUG(msg, ...)                                    \
  ZNET_PRINTFN("\x1b[44m[debug]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, __VA_ARGS__)
#else
#define ZNET_LOG_DEBUG(msg, ...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_INFO
#define ZNET_LOG_INFO(msg, ...)                                     \
  ZNET_PRINTFN("\x1b[42m[info ]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, __VA_ARGS__)
#else
#define ZNET_LOG_INFO(msg, ...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_WARN
#define ZNET_LOG_WARN(msg, ...)                                      \
  ZNET_PRINTFN("\x1b[41m[warn ]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, __VA_ARGS__)
#else
#define ZNET_LOG_WARN(msg, ...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_ERROR
#define ZNET_LOG_ERROR(msg, ...)                                     \
  ZNET_PRINTFN("\x1b[41m[error]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, __VA_ARGS__)
#else
#define ZNET_LOG_ERROR(msg, ...)
#endif
#else
#define ZNET_PRINTFN(fmsg, func, msg, args...)               \
  fmt::print(fmsg, func, fmt::format(msg, ##args)); \
  std::cout << std::flush

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_DEBUG
#define ZNET_LOG_DEBUG(msg, args...)                                    \
  ZNET_PRINTFN("\x1b[44m[debug]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, ##args)
#else
#define ZNET_LOG_DEBUG(msg, args...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_INFO
#define ZNET_LOG_INFO(msg, args...)                                     \
  ZNET_PRINTFN("\x1b[42m[info ]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, ##args)
#else
#define ZNET_LOG_INFO(msg, args...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_WARN
#define ZNET_LOG_WARN(msg, args...)                                          \
  ZNET_PRINTFN("\x1b[41m[warn ]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, ##args)
#else
#define ZNET_LOG_WARN(msg, args...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_ERROR
#define ZNET_LOG_ERROR(msg, ...)                                         \
  ZNET_PRINTFN("\x1b[41m[error]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n", \
               ZNET_FUNC_SIGN, msg, ##args)
#else
#define ZNET_LOG_ERROR(msg, args...)
#endif
#endif

class LoggerInitializer {
 public:
  static bool s_Initialized;

  LoggerInitializer();
};

static LoggerInitializer s_LoggerInitializer;