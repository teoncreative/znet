//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "znet/compat.h"
#include "znet/ext/delta/sequence.h"

namespace znet {
namespace ext {

/**
 * @brief Keeps recent snapshots so a delta can be built against one the peer has.
 *
 * A delta is only decodable if the receiver holds the exact baseline it was
 * built from. Over a lossy link the newest snapshot is often not that
 * baseline, so the sender keeps a window of them and encodes against the
 * newest one the peer has acknowledged.
 *
 * The sender's loop:
 *
 * @code
 *   history.Store(sequence, snapshot);
 *   const Snapshot* baseline = history.AcknowledgedSnapshot();
 *   if (baseline != nullptr) {
 *     WriteDelta(buffer, snapshot, *baseline, history.acknowledged());
 *   } else {
 *     WriteFull(buffer, snapshot);   // nothing to delta against
 *   }
 * @endcode
 *
 * and on every ack from the peer, `history.Acknowledge(their_sequence)`.
 *
 * AcknowledgedSnapshot() returning nullptr is the normal, expected signal to
 * send a full snapshot: it means the peer has acknowledged nothing yet, or has
 * been quiet long enough that its baseline has aged out of the window. A
 * sender that treats it as an error stalls; one that ignores it and deltas
 * against something else corrupts the receiver silently.
 *
 * @tparam Capacity Snapshots retained. At 60 Hz, 64 covers about a second,
 *         which is a round trip plus a comfortable margin.
 */
template <typename T, size_t Capacity = 64, typename Sequence = uint16_t>
class SnapshotHistory {
  static_assert(Capacity > 0, "a history needs room for at least one snapshot");

 public:
  using SequenceType = Sequence;

  /** @brief Records @p snapshot, evicting whatever shared its slot. */
  void Store(Sequence sequence, const T& snapshot) {
    Slot& slot = slots_[static_cast<size_t>(sequence) % Capacity];
    slot.sequence = sequence;
    slot.occupied = true;
    slot.value = snapshot;
  }

  /**
   * @brief The snapshot stored under @p sequence, or nullptr if it is gone.
   *
   * The slot carries its own sequence number, so a wrapped-around counter
   * landing on an old slot reports a miss rather than the wrong snapshot.
   */
  ZNET_NODISCARD const T* Find(Sequence sequence) const {
    const Slot& slot = slots_[static_cast<size_t>(sequence) % Capacity];
    if (!slot.occupied || slot.sequence != sequence) {
      return nullptr;
    }
    return &slot.value;
  }

  /**
   * @brief Records that the peer has this snapshot.
   *
   * Acks arrive out of order on an unreliable link, so an older one never
   * moves the mark backwards.
   */
  void Acknowledge(Sequence sequence) {
    if (!acknowledged_ || SequenceGreaterThan(sequence, acknowledged_value_)) {
      acknowledged_ = true;
      acknowledged_value_ = sequence;
    }
  }

  /** @brief Whether the peer has acknowledged anything at all. */
  ZNET_NODISCARD bool has_acknowledged() const { return acknowledged_; }

  /** @brief The newest sequence the peer has acknowledged. */
  ZNET_NODISCARD Sequence acknowledged() const { return acknowledged_value_; }

  /**
   * @brief The acknowledged snapshot, or nullptr when there is no usable baseline.
   *
   * nullptr means "send a full snapshot", not "something went wrong".
   */
  ZNET_NODISCARD const T* AcknowledgedSnapshot() const {
    if (!acknowledged_) {
      return nullptr;
    }
    return Find(acknowledged_value_);
  }

  /** @brief Drops every stored snapshot and forgets the ack. */
  void Reset() {
    for (size_t i = 0; i < Capacity; ++i) {
      slots_[i].occupied = false;
    }
    acknowledged_ = false;
    acknowledged_value_ = Sequence{};
  }

  ZNET_NODISCARD static constexpr size_t capacity() { return Capacity; }

 private:
  struct Slot {
    T value{};
    Sequence sequence{};
    bool occupied = false;
  };

  std::array<Slot, Capacity> slots_{};
  Sequence acknowledged_value_{};
  bool acknowledged_ = false;
};

}  // namespace ext
}  // namespace znet
