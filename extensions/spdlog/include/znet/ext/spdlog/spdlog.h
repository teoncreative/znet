//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// routes znet's logging into spdlog.
//
//   #include "znet/ext/spdlog/spdlog.h"
//
// znet has its own small logger, which is the right default for a library: no
// dependency, and it stays out of the way. An application that already runs
// spdlog wants one place where logs are filtered, formatted and shipped, and
// znet's output arriving on std::cout instead is a nuisance.
//
// this connects the two through znet::SetLogSink, so severity survives the
// trip. Routing the *stream* instead would work, but by then a record is one
// string with the level rendered into a coloured prefix, and spdlog would file
// everything at whatever level you picked for the whole feed.
//

#pragma once

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "znet/logger.h"

namespace znet {
namespace ext {

namespace detail {

/**
 * @brief The bridges currently installed, oldest first.
 *
 * Destruction order does not matter: a bridge removes itself from wherever it
 * sits and whatever is left on top becomes current, so where records go
 * depends on which bridges exist rather than the order they went away in. A
 * raw pointer to the displaced sink could not manage that, since nothing about
 * it says whether its owner is still alive.
 *
 * Only install and uninstall touch this; logging stays a single relaxed load.
 */
struct BridgeRegistry {
  std::mutex mutex;
  std::vector<const LogSink*> installed;
  /** @brief Whatever was routed to before the first bridge appeared. */
  const LogSink* original = nullptr;
};

inline BridgeRegistry& Bridges() {
  static BridgeRegistry registry;
  return registry;
}

}  // namespace detail

/**
 * @brief Sends znet's log records to a spdlog logger while it is alive.
 *
 * @code
 *   znet::ext::SpdlogBridge bridge{spdlog::default_logger()};
 *   // ... run your server; znet logs now arrive through spdlog
 * @endcode
 *
 * Installs on construction and stands down on destruction, so it can be scoped
 * to the part of the program that wants it. Keep it alive at least as long as
 * anything that logs: znet holds a plain pointer to it, exactly as
 * SetLogStream holds one to a stream.
 *
 * Bridges nest, and they need not be destroyed in the order they were created.
 * Destroying one simply takes it out of the running; whichever others are
 * still alive keep working. See detail::BridgeRegistry for why that is not
 * just a saved pointer.
 *
 * Records can arrive on any znet thread. spdlog's `_mt` loggers are safe for
 * that; an `_st` logger is not, and neither is this with one.
 */
class SpdlogBridge {
 public:
  explicit SpdlogBridge(std::shared_ptr<spdlog::logger> logger,
                        bool include_function = false)
      : logger_(std::move(logger)), include_function_(include_function) {
    sink_.write = &SpdlogBridge::Write;
    sink_.user = this;

    detail::BridgeRegistry& bridges = detail::Bridges();
    std::lock_guard<std::mutex> lock(bridges.mutex);
    if (bridges.installed.empty()) {
      bridges.original = LogSinkPtr().load(std::memory_order_relaxed);
    }
    bridges.installed.push_back(&sink_);
    SetLogSink(&sink_);
  }

  ~SpdlogBridge() {
    detail::BridgeRegistry& bridges = detail::Bridges();
    std::lock_guard<std::mutex> lock(bridges.mutex);
    bridges.installed.erase(std::remove(bridges.installed.begin(),
                                        bridges.installed.end(), &sink_),
                            bridges.installed.end());
    // whatever is left on top wins. Nested scopes unwind to the enclosing
    // bridge; destroying one out of order just removes it from the middle and
    // leaves the rest routed, rather than reinstating something freed.
    SetLogSink(bridges.installed.empty() ? bridges.original
                                         : bridges.installed.back());
  }

  SpdlogBridge(const SpdlogBridge&) = delete;
  SpdlogBridge& operator=(const SpdlogBridge&) = delete;

  /** @brief The logger records are going to. */
  const std::shared_ptr<spdlog::logger>& logger() const { return logger_; }

  /** @brief Maps a znet severity onto spdlog's. */
  static spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
      case LogLevel::Debug:
        return spdlog::level::debug;
      case LogLevel::Info:
        return spdlog::level::info;
      case LogLevel::Warn:
        return spdlog::level::warn;
      case LogLevel::Error:
        return spdlog::level::err;
    }
    return spdlog::level::info;
  }

 private:
  static void Write(LogLevel level, const char* function, const char* message,
                    void* user) {
    auto* self = static_cast<SpdlogBridge*>(user);
    if (self == nullptr || !self->logger_) {
      return;
    }
    const spdlog::level::level_enum mapped = ToSpdlogLevel(level);
    if (self->include_function_ && function != nullptr) {
      // __PRETTY_FUNCTION__ is a whole signature, so this is off by default:
      // it is genuinely useful when chasing something and unreadable the rest
      // of the time.
      self->logger_->log(mapped, "{}: {}", function, message);
    } else {
      self->logger_->log(mapped, "{}", message);
    }
  }

  std::shared_ptr<spdlog::logger> logger_;
  LogSink sink_;
  bool include_function_ = false;
};

}  // namespace ext
}  // namespace znet
