
//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"
#include "znet/types.h"

#include <atomic>
#include <thread>
#include <utility>

namespace znet {

class Task {
 public:
  Task() = default;

  ~Task() {
    RequestStop();
    Wait();
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept
      : thread_(std::move(other.thread_)),
        stop_requested_(other.stop_requested_.load()) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      RequestStop();
      Wait();
      thread_ = std::move(other.thread_);
      stop_requested_.store(other.stop_requested_.load());
    }
    return *this;
  }

  ZNET_NODISCARD bool IsRunning() const {
    return thread_.joinable();
  }

  ZNET_NODISCARD bool IsStopRequested() const {
    return stop_requested_.load(std::memory_order_acquire);
  }

  void Run(std::function<void()> run) {
    RequestStop();
    Wait();
    stop_requested_.store(false, std::memory_order_release);
    thread_ = std::thread(std::move(run));
  }

  void RequestStop() {
    stop_requested_.store(true, std::memory_order_release);
  }

  void Wait() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
};

}