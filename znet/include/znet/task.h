
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

#include <thread>
#include <utility>

namespace znet {
class Task {
 public:
  Task() {
    thread_finished_ = true;
  }
  ~Task() {
    Wait();
  }

  Task(const Task&) = delete;

  Task& operator=(const Task&) = delete;

  ZNET_NODISCARD bool IsRunning() const {
    return thread_ != nullptr;
  }

  void Run(std::function<void()> run) {
    thread_ = std::make_unique<std::thread>([this, run = std::move(run)]() {
      run();
      SignalThreadFinished();
    });
    thread_finished_ = false;
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return thread_finished_; });  // Wait for the signal that thread is finished
    if (thread_ && thread_->joinable()) {
      thread_->join();  // Safely join the thread
      thread_ = nullptr;  // Now it's safe to reset the pointer
    }
  }

 private:
  std::unique_ptr<std::thread> thread_;

  std::mutex mtx_;  // Mutex for synchronization
  std::condition_variable cv_;  // Condition variable for signaling
  bool thread_finished_ = false;  // Flag to indicate thread completion

 private:
  void SignalThreadFinished() {
    std::lock_guard<std::mutex> lock(mtx_);
    thread_finished_ = true;
    cv_.notify_one();  // Notify waiting function
  }
};

}
