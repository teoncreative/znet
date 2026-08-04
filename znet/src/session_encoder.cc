//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/session_encoder.h"

namespace znet {

void SessionEncoder::Start(std::shared_ptr<PeerSession> session,
                           std::function<void()> wake_flusher) {
  if (task_.IsRunning() || !session) {
    return;
  }
  session_ = std::move(session);
  wake_flusher_ = std::move(wake_flusher);

  auto signal = signal_;
  auto wake = wake_flusher_;
  session_->SetWakeCallback([signal, wake]() {
    signal->Raise();
    if (wake) {
      wake();
    }
  });
  // set before the thread exists, so the flusher never encodes a deep queue
  // this encoder was about to take
  session_->SetHasDedicatedEncoder(true);
  task_.Run([this]() { Loop(); });
}

void SessionEncoder::Loop() {
  signal_->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
  while (!task_.IsStopRequested()) {
    // only nudge the flusher when there was something to flush; it often wins
    // the claim and encodes the message itself
    if (session_->DrainOutbound() && wake_flusher_) {
      wake_flusher_();
    }
    std::unique_lock<std::mutex> lock(signal_->mutex);
    signal_->cv.wait(lock, [this]() {
      return signal_->woken.load(std::memory_order_relaxed) ||
             task_.IsStopRequested();
    });
    signal_->woken.store(false, std::memory_order_relaxed);
  }
}

void SessionEncoder::Stop() {
  if (!task_.IsRunning()) {
    return;
  }
  // the stop flag is part of the wait predicate, so it is set with the mutex
  // held: signaling outside it lets an encoder between testing the predicate
  // and sleeping miss the wake and never return.
  {
    std::lock_guard<std::mutex> lock(signal_->mutex);
    task_.RequestStop();
  }
  signal_->cv.notify_all();
  task_.Wait();
  // the session's own loop owns encoding again now that nothing else drains
  if (session_) {
    session_->SetHasDedicatedEncoder(false);
  }
  // a stopped encoder has no use for the session, and holding on would keep
  // the transport's descriptor (and so its port) alive with it
  session_ = nullptr;
}

}  // namespace znet
