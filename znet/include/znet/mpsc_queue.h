//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PARENT_MPSC_QUEUE_H
#define ZNET_PARENT_MPSC_QUEUE_H

#include "znet/precompiled.h"

#include <atomic>
#include <cstddef>
#include <limits>
#include <utility>

namespace znet {

/**
 * @brief Many-producer, one-consumer bounded queue.
 *
 * Vyukov's bounded MPMC ring, with the consumer side reduced to plain loads
 * and stores and a wake handshake added: Push() reports the depth it queued
 * behind and Empty() errs towards non-empty, so a producer and a sleeping
 * consumer cannot both decide the other will deal with a message. See both.
 *
 * Bounded and preallocated so the send path never enters the allocator, and so
 * capacity is itself the backpressure signal: a full ring refuses, where a
 * linked queue needs a counter alongside it to know its own length.
 *
 * Only one thread may consume. Nothing checks that.
 *
 * @tparam T must be default-constructible and move-assignable. Draining moves
 *         out of the slot, so a T owning a resource releases it there rather
 *         than holding it until the slot is written a lap later.
 */
template <typename T>
class MpscQueue {
 public:
  /** @brief Allocates `capacity` rounded up to a power of two, minimum two. */
  explicit MpscQueue(size_t capacity) {
    const size_t slots = RoundUpPowerOfTwo(capacity);
    mask_ = slots - 1;
    cells_ = new Cell[slots];
    for (size_t i = 0; i < slots; i++) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
    enqueue_.value.store(0, std::memory_order_relaxed);
    dequeue_.value.store(0, std::memory_order_relaxed);
  }

  ~MpscQueue() { delete[] cells_; }

  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;

  /**
   * @brief Appends one value. Callable from any thread.
   *
   * @param value      moved into the ring on success, left alone on failure.
   * @param out_queued receives how many items were queued ahead of this one,
   *                   zero meaning the queue had been drained empty. Pass null
   *                   to skip the read of the consumer's cursor, which sits in
   *                   another core's cache.
   * @return false when the ring is full.
   */
  bool Push(T value, size_t* out_queued = nullptr) {
    Cell* cell;
    size_t pos = enqueue_.value.load(std::memory_order_relaxed);
    for (;;) {
      cell = &cells_[pos & mask_];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      // unsigned wraparound gives the signed distance without signed overflow
      const size_t distance = sequence - pos;
      if (distance == 0) {
        // seq_cst is for the wake handshake, not the payload, which the
        // release store at the end publishes. See out_queued below.
        if (enqueue_.value.compare_exchange_weak(pos, pos + 1,
                                                 std::memory_order_seq_cst,
                                                 std::memory_order_relaxed)) {
          break;
        }
      } else if (distance > kBehind) {
        return false;  // full: this slot still holds the previous lap's item
      } else {
        pos = enqueue_.value.load(std::memory_order_relaxed);
      }
    }
    if (out_queued != nullptr) {
      // read before publishing: the consumer can take this position the moment
      // the sequence store lands, leaving the cursor past it.
      //
      // seq_cst, paired with Empty(). under acquire/release each side can miss
      // the other's last move, the producer assuming a drain is already coming
      // and the consumer assuming nothing is left, which parks the message for
      // a tick with both asleep. callers that wake nobody pass null.
      *out_queued = pos - dequeue_.value.load(std::memory_order_seq_cst);
    }
    cell->value = std::move(value);
    cell->sequence.store(pos + 1, std::memory_order_release);  // see Pop()
    return true;
  }

  /** @brief Takes the oldest item. Consumer thread only. */
  bool Pop(T& out) {
    Cell* cell = NextReadable();
    if (cell == nullptr) {
      return false;
    }
    out = std::move(cell->value);
    ReleaseRead(cell);
    return true;
  }

  /**
   * @brief Moves everything readable into `out`, oldest first.
   *
   * Stops at the first position a producer has claimed but not yet stored
   * into; that item and its successors come out on the next drain.
   *
   * @return how many items were appended to `out`.
   */
  template <typename Container>
  size_t DrainTo(Container& out) {
    size_t count = 0;
    while (Cell* cell = NextReadable()) {
      out.push_back(std::move(cell->value));
      ReleaseRead(cell);
      count++;
    }
    return count;
  }

  /**
   * @brief Whether anything is queued, counting a position claimed but not yet
   *        stored into.
   *
   * The consumer's half of the wake handshake, the check it makes before it
   * stops draining, so erring towards non-empty is deliberate. See Push()'s
   * out_queued for the other half.
   *
   * Consumer thread only, and not const: it republishes the read cursor, which
   * is what orders this check against a producer's claim. A store rather than
   * a standalone fence because GCC rejects std::atomic_thread_fence under
   * -fsanitize=thread.
   */
  ZNET_NODISCARD bool Empty() {
    const size_t head = dequeue_.value.load(std::memory_order_relaxed);
    dequeue_.value.store(head, std::memory_order_seq_cst);
    return enqueue_.value.load(std::memory_order_seq_cst) == head;
  }

  /** @brief How many items are queued. A sample, not a fence: both cursors
   *  move under it. */
  ZNET_NODISCARD size_t size() const {
    const size_t head = dequeue_.value.load(std::memory_order_relaxed);
    const size_t tail = enqueue_.value.load(std::memory_order_relaxed);
    // the two loads are independent, so a stale one can order them backwards
    return tail > head ? tail - head : 0;
  }

  /** @brief Items the ring holds before Push() starts refusing. */
  ZNET_NODISCARD size_t capacity() const { return mask_ + 1; }

 private:
  // 64 on x86 and Zen. Parts with 128-byte lines (Apple silicon) may still
  // share one between the two cursors; nothing else is lost there.
  static constexpr size_t kCacheLine = 64;

  // a `distance` above this wrapped around, i.e. the sequence is behind the
  // position rather than ahead of it
  static constexpr size_t kBehind = (std::numeric_limits<size_t>::max)() >> 1;

  struct Cell {
    Cell() : sequence(0), value() {}
    std::atomic<size_t> sequence;
    T value;
  };

  // a line each: both are written on every operation, by different threads.
  // The padding leads rather than trails so whatever is declared before a
  // cursor, namely cells_ and mask_ which both sides read constantly, cannot
  // share its line either.
  struct Cursor {
    char padding[kCacheLine];
    std::atomic<size_t> value;
  };

  static size_t RoundUpPowerOfTwo(size_t value) {
    // stops at half of what a size_t holds, which no ring could allocate
    const size_t limit = ((std::numeric_limits<size_t>::max)() >> 1) + 1;
    size_t slots = 2;
    while (slots < value && slots < limit) {
      slots <<= 1;
    }
    return slots;
  }

  /** @brief The slot holding the oldest item, or null when there is none
   *  readable yet. */
  Cell* NextReadable() {
    const size_t pos = dequeue_.value.load(std::memory_order_relaxed);
    Cell* cell = &cells_[pos & mask_];
    if (cell->sequence.load(std::memory_order_acquire) != pos + 1) {
      return nullptr;
    }
    return cell;
  }

  /** @brief Hands the slot NextReadable() returned back to the producers. */
  void ReleaseRead(Cell* cell) {
    const size_t pos = dequeue_.value.load(std::memory_order_relaxed);
    // release: a producer must not see the slot free before the move out of it
    // has finished reading it
    cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
    dequeue_.value.store(pos + 1, std::memory_order_relaxed);
  }

  Cell* cells_;
  size_t mask_;
  Cursor enqueue_;  // producers claim positions here
  Cursor dequeue_;  // the consumer advances this
};

}  // namespace znet

#endif  // ZNET_PARENT_MPSC_QUEUE_H
