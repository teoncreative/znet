
//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_LOGGER_H_
#define ZNET_LOGGER_H_

#include "znet/precompiled.h"
#include <atomic>
#include <iostream>
#include <ostream>

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

namespace znet {

/**
 * @brief Where log output goes. std::cout until SetLogStream() says otherwise.
 *
 * ZNET_LOG_LEVEL is a compile-time switch and only affects the translation unit
 * that defines it, so it cannot quiet a znet built separately. This can: it is
 * read by the library's own log calls too.
 */
inline std::atomic<std::ostream*>& LogStreamPtr() {
  static std::atomic<std::ostream*> stream{&std::cout};
  return stream;
}

/**
 * @brief Sends all znet logging to @p stream instead of std::cout.
 *
 * For applications that own the terminal, a full-screen TUI above all, where a
 * stray log line lands in the middle of the interface. Point this at a file and
 * the display stays yours.
 *
 * @p stream must outlive every later log call, so a namespace-scope or
 * function-static stream rather than a local one. Set it before starting a
 * server or client: the pointer is atomic, so changing it while other threads
 * log is safe, but the change is not ordered against them and a line already in
 * flight may still reach the old stream.
 */
inline void SetLogStream(std::ostream& stream) {
  LogStreamPtr().store(&stream, std::memory_order_relaxed);
}

/** @brief The stream logging currently writes to. */
inline std::ostream& LogStream() {
  return *LogStreamPtr().load(std::memory_order_relaxed);
}

/** @brief Severity of a log record, matching the ZNET_LOG_LEVEL_* constants. */
enum class LogLevel {
  Debug = ZNET_LOG_LEVEL_DEBUG,
  Info = ZNET_LOG_LEVEL_INFO,
  Warn = ZNET_LOG_LEVEL_WARN,
  Error = ZNET_LOG_LEVEL_ERROR,
};

/**
 * @brief Receives log records instead of the stream, with the severity intact.
 *
 * SetLogStream is enough to move znet's output somewhere else, but by the time
 * a line reaches a stream it is one string: the severity has been rendered into
 * a colored prefix, and a logging library on the far side would have to parse
 * it back out to file the record correctly. A sink is handed the level, the
 * function and the message as separate values, with no ANSI escapes, so it can
 * hand them straight to spdlog, a structured logger, or a TUI's log pane.
 *
 * @p message is owned by the caller and only valid for the duration of the
 * call; copy it to keep it. Records can arrive from any znet thread, so a sink
 * that writes anywhere shared has to do its own locking.
 */
struct LogSink {
  void (*write)(LogLevel level, const char* function, const char* message,
                void* user) = nullptr;
  void* user = nullptr;
};

inline std::atomic<const LogSink*>& LogSinkPtr() {
  static std::atomic<const LogSink*> sink{nullptr};
  return sink;
}

/**
 * @brief Routes logging to @p sink instead of the stream. Null restores it.
 *
 * @p sink must outlive every later log call, so a namespace-scope or
 * function-static object rather than a local one, exactly as with
 * SetLogStream. A single atomic pointer is stored rather than a std::function
 * so that swapping sinks cannot be observed half-applied.
 */
inline void SetLogSink(const LogSink* sink) {
  LogSinkPtr().store(sink, std::memory_order_relaxed);
}

}  // namespace znet

// the message is folded into __VA_ARGS__ rather than named separately, which
// is what removes the need for __VA_OPT__ (C++20) to elide the comma when a
// log call passes no arguments beyond the message.
//
// the formatting stays inside the macro rather than moving into a function
// that both paths could call: std::format takes its format string as a
// consteval parameter, so `fmsg` has to still be the literal from the call
// site. Passing it along as a const char* would compile only on the fmtlib and
// C++14 shim backends and break the C++20 one.
//
// cost when no sink is installed is one relaxed load and a predictable branch,
// against a format call and a stream write that were happening anyway.
#define ZNET_PRINTFN(lvl, fmsg, func, ...)                                     \
  do {                                                                         \
    const ::znet::LogSink* znet_sink_ =                                        \
        ::znet::LogSinkPtr().load(::std::memory_order_relaxed);                \
    if (znet_sink_ != nullptr && znet_sink_->write != nullptr) {               \
      const ::std::string znet_message_ = ZNET_FORMAT(__VA_ARGS__);            \
      znet_sink_->write((lvl), (func), znet_message_.c_str(),                  \
                        znet_sink_->user);                                     \
    } else {                                                                   \
      ::znet::LogStream() << ZNET_FORMAT(fmsg, func, ZNET_FORMAT(__VA_ARGS__)) \
                          << ::std::flush;                                     \
    }                                                                          \
  } while (false)

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_DEBUG
#define ZNET_LOG_DEBUG(...)                                             \
ZNET_PRINTFN(::znet::LogLevel::Debug,                                   \
"\x1b[44m[debug]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n",   \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_DEBUG(...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_INFO
#define ZNET_LOG_INFO(...)                                              \
ZNET_PRINTFN(::znet::LogLevel::Info,                                   \
"\x1b[42m[info ]\x1b[0m \x1b[35m{}: \x1b[0m{}\x1b[0m\n",   \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_INFO(...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_WARN
#define ZNET_LOG_WARN(...)                                              \
ZNET_PRINTFN(::znet::LogLevel::Warn,                                   \
"\x1b[41m[warn ]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n",   \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_WARN(...)
#endif

#if ZNET_LOG_LEVEL <= ZNET_LOG_LEVEL_ERROR
#define ZNET_LOG_ERROR(...)                                             \
ZNET_PRINTFN(::znet::LogLevel::Error,                                   \
"\x1b[41m[error]\x1b[0m \x1b[35m{}: \x1b[31m{}\x1b[0m\n",   \
ZNET_FUNC_SIGN, __VA_ARGS__)
#else
#define ZNET_LOG_ERROR(...)
#endif

namespace znet {

class LoggerInitializer {
 public:
  static bool s_Initialized;

  LoggerInitializer();
};

// one per translation unit on purpose: whichever TU logs first has already
// run its initializer, and s_Initialized keeps the work single-shot
static LoggerInitializer s_LoggerInitializer;

}  // namespace znet

#endif  // ZNET_LOGGER_H_
