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
// The hand-off between whoever sends a packet and whoever encodes it. Three
// things that only make sense together: the queue itself, the claim that keeps
// exactly one thread encoding at a time, and the rule deciding which thread
// should take it. They used to sit loose on the session, which is how two
// threads came to race for the claim with nothing choosing between them.
//

#ifndef ZNET_PARENT_OUTBOUND_QUEUE_H
#define ZNET_PARENT_OUTBOUND_QUEUE_H

#include "znet/mpsc_queue.h"
#include "znet/packet.h"
#include "znet/precompiled.h"
#include "znet/send_options.h"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace znet {

/**
 * @brief Packets waiting to be encoded, and the arbitration over who encodes.
 *
 * @par Threading
 * Push() is callable from any thread. Drain() may be called from any thread and
 * only one succeeds at a time; the rest return immediately rather than waiting,
 * so nothing blocks here.
 */
class OutboundQueue {
 public:
  struct Item {
    std::shared_ptr<Packet> packet;
    SendOptions options;
  };

  explicit OutboundQueue(size_t capacity) : queue_(capacity) {}

  OutboundQueue(const OutboundQueue&) = delete;
  OutboundQueue& operator=(const OutboundQueue&) = delete;

  /**
   * @brief Queues one packet. Any thread.
   *
   * Wakes the encoder only on the idle edge: with anything already queued a
   * drain is on its way and the notify would be pure overhead on the hot path.
   *
   * @return false when the queue is full, which is the backpressure signal. The
   *         caller still owns the packet and may retry.
   */
  bool Push(std::shared_ptr<Packet> packet, SendOptions options) {
    size_t queued = 0;
    if (!queue_.Push(Item{std::move(packet), options}, &queued)) {
      return false;
    }
    if (queued == 0 && wake_) {
      wake_();
    }
    return true;
  }

  /**
   * @brief Encodes everything queued, if this thread wins the claim.
   *
   * @param encode called once per item while the claim is held, and returns
   *        whether it actually encoded. Draining continues either way, so a
   *        session that has died still releases what it queued instead of
   *        holding it until destruction.
   * @return whether anything was encoded, so a caller can skip waking the
   *         thread that flushes when there was nothing to flush.
   */
  template <typename EncodeFn>
  bool Drain(EncodeFn&& encode) {
    // whoever takes the claim encodes; whoever does not returns rather than
    // waiting, so no thread blocks here.
    if (encoding_.exchange(true, std::memory_order_acquire)) {
      return false;
    }
    bool encoded = false;
    // one at a time, so the slot is free before the encode starts and a
    // producer gets it back sooner. The loop runs until the queue is empty, so
    // anything queued while it works still goes out before the claim is
    // released.
    Item item;
    while (queue_.Pop(item)) {
      if (encode(item)) {
        encoded = true;
      }
    }
    encoding_.store(false, std::memory_order_release);
    // a packet pushed between the last Pop() and the release raised no wake of
    // its own, its producer having seen a non-zero count, so nudge here rather
    // than let it wait out a tick.
    if (!queue_.Empty() && wake_) {
      wake_();
    }
    return encoded;
  }

  /**
   * @brief Whether the worker should encode this tick rather than leave it.
   *
   * With no dedicated encoder the worker is the only candidate. With one, a
   * shallow queue is still encoded on the worker, because that is the latency
   * path and waking another thread costs more than the encode; anything deeper
   * is left to the encoder so encoding overlaps the flush instead of
   * serializing in front of it.
   *
   * Both threads simply racing for the claim is worse than either rule:
   * whichever won the first round decided the shape of the whole connection,
   * which showed up as throughput settling into one of two regimes.
   */
  ZNET_NODISCARD bool ShouldEncodeInline() const {
    return !has_encoder_.load(std::memory_order_relaxed) ||
           queue_.size() <= kInlineEncodeDepth;
  }

  /**
   * @brief Registers the callback that ends an idle worker's tick sleep.
   *
   * Set once before the session is visible to the application and never
   * replaced, so Push() reads it without synchronizing.
   */
  void SetWakeCallback(std::function<void()> wake) { wake_ = std::move(wake); }

  /** @brief Declares that a thread other than the worker drains this queue. */
  void SetHasDedicatedEncoder(bool has_encoder) {
    has_encoder_.store(has_encoder, std::memory_order_relaxed);
  }

  ZNET_NODISCARD size_t size() const { return queue_.size(); }
  ZNET_NODISCARD size_t capacity() const { return queue_.capacity(); }

 private:
  // queue depth up to which the worker encodes even when a dedicated encoder
  // exists. One message is the latency case, and handing it to another thread
  // costs a wake to save nothing.
  static constexpr size_t kInlineEncodeDepth = 1;

  // lock-free because Push() runs on the application's thread, where blocking
  // on a worker mid-encode would cost a frame.
  MpscQueue<Item> queue_;
  // held by whichever thread is encoding. Not a lock: a thread that finds it
  // taken returns rather than waiting. Keeps message order defined while
  // letting any thread encode.
  std::atomic<bool> encoding_{false};
  std::atomic<bool> has_encoder_{false};
  std::function<void()> wake_;
};

}  // namespace znet

#endif  // ZNET_PARENT_OUTBOUND_QUEUE_H
