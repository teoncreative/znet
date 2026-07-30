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
// What a session-driving loop sleeps on between ticks, and the handle anything
// wanting to cut that sleep short holds. Both the server's workers and the
// client's single loop use it, so a session behaves the same either side.
//
// Held by shared_ptr because a session can outlive the loop that drove it: the
// application may keep a shared_ptr to one past shutdown, and its wake callback
// would otherwise be left pointing at freed state.
//

#ifndef ZNET_PARENT_WORKER_SIGNAL_H
#define ZNET_PARENT_WORKER_SIGNAL_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace znet {

struct WorkerSignal {
  std::mutex mutex;
  std::condition_variable cv;
  // raised by a backend's receive thread, or by Send() on an idle session, to
  // end the tick sleep early.
  std::atomic_bool woken{false};
  // the loop's own thread, so work it queues itself raises no wake. there is
  // nothing asleep to interrupt, PeerSession::Process already flushes replies,
  // and setting the flag would skip the rest of the tick and spin for as long
  // as the session had traffic.
  std::atomic<std::thread::id> owner{};

  /**
   * @brief Ends the sleep unless called from the loop's own thread.
   *
   * The flag is set under the mutex so it cannot be missed by a sleeper
   * between testing the predicate and waiting. That obliges every loop using
   * this to release the mutex while it works. One holding it across
   * processing would block its notifiers for a whole tick.
   */
  void Raise() {
    if (std::this_thread::get_id() == owner.load(std::memory_order_relaxed)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      woken.store(true, std::memory_order_relaxed);
    }
    cv.notify_one();
  }
};

}  // namespace znet

#endif  // ZNET_PARENT_WORKER_SIGNAL_H
