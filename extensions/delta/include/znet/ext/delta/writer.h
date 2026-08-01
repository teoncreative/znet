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

#include <cstdint>

#include "znet/compat.h"
#include "znet/ext/bitpack/bitpack.h"

namespace znet {
namespace ext {

/**
 * @brief Writes only the fields that differ from a baseline the peer already has.
 *
 * Each field spends one bit saying whether it changed, and its value only if
 * it did. In a typical tick most of an entity is static -- an idle player's
 * ammo, health, team and weapon do not move -- so the packet collapses to the
 * handful of fields that actually did something.
 *
 * @code
 *   BitWriter bits(*buffer);
 *   DeltaWriter delta(bits);
 *   delta.WriteUIntRanged(now.ammo,   was.ammo,   0, 200);
 *   delta.WriteIntRanged (now.health, was.health, -50, 100);
 *   delta.WriteFloatRanged(now.yaw,   was.yaw,   -3.15f, 3.15f, 12);
 *   delta.WriteBool(now.firing);
 * @endcode
 *
 * Unchanged, that entity costs 4 bits instead of 29.
 *
 * The baseline must be the snapshot the peer has *acknowledged*, not simply
 * the previous one -- see SnapshotHistory. Sending a delta against a snapshot
 * that never arrived produces a receiver that is confidently wrong, which is
 * far worse than a dropped packet, and no amount of checking on the receiving
 * end can detect it.
 */
class DeltaWriter {
 public:
  explicit DeltaWriter(BitWriter& bits) : bits_(bits) {}

  DeltaWriter(const DeltaWriter&) = delete;
  DeltaWriter& operator=(const DeltaWriter&) = delete;

  /**
   * @brief Writes a bool, always in full.
   *
   * Deliberately takes no baseline. A changed flag costs one bit and so does
   * the value, so delta encoding a bool can only ever make it bigger. Taking
   * the parameter and ignoring it would just invite the reader of this code to
   * assume otherwise.
   */
  void WriteBool(bool value) {
    ++fields_;
    ++changed_;
    bits_.WriteBool(value);
  }

  /** @brief One bit if @p value matches @p baseline, otherwise that plus the field. */
  void WriteUIntRanged(uint64_t value, uint64_t baseline, uint64_t min,
                       uint64_t max) {
    // clamp both sides: the receiver's baseline is a value it decoded, and
    // decoding always clamps, so comparing raw inputs could disagree with it
    const uint64_t current = compat::Clamp(value, min, max);
    const uint64_t previous = compat::Clamp(baseline, min, max);
    if (Flag(current != previous)) {
      bits_.WriteUIntRanged(current, min, max);
    }
  }

  /** @brief WriteUIntRanged for a signed range. */
  void WriteIntRanged(int64_t value, int64_t baseline, int64_t min,
                      int64_t max) {
    const int64_t current = compat::Clamp(value, min, max);
    const int64_t previous = compat::Clamp(baseline, min, max);
    if (Flag(current != previous)) {
      bits_.WriteIntRanged(current, min, max);
    }
  }

  /**
   * @brief One bit if @p value quantises to the same code as @p baseline.
   *
   * The comparison is between quantised codes, not floats, and that is the
   * whole trick. What the receiver holds is a dequantised code, so "unchanged"
   * has to mean "the code is unchanged". Comparing the raw floats instead
   * would send a field every tick for a value drifting inside one quantisation
   * step -- and, worse, for a caller whose baseline is its own full-precision
   * value rather than what it actually transmitted, it would skip fields the
   * receiver needed.
   */
  void WriteFloatRanged(float value, float baseline, float min, float max,
                        unsigned bits) {
    const uint32_t current = detail::QuantizeFloat(value, min, max, bits);
    const uint32_t previous = detail::QuantizeFloat(baseline, min, max, bits);
    if (Flag(current != previous)) {
      bits_.WriteBits(current, bits);
    }
  }

  /** @brief Delta of an opaque fixed-width field: ids, enums, bit flags. */
  void WriteBits(uint32_t value, uint32_t baseline, unsigned bits) {
    if (Flag(value != baseline)) {
      bits_.WriteBits(value, bits);
    }
  }

  /** @brief Fields offered so far. */
  ZNET_NODISCARD size_t fields_written() const { return fields_; }

  /** @brief How many of them actually had to be sent. */
  ZNET_NODISCARD size_t fields_changed() const { return changed_; }

 private:
  /** @brief Writes the changed flag, counts it, and reports it back. */
  bool Flag(bool changed) {
    ++fields_;
    bits_.WriteBool(changed);
    if (changed) {
      ++changed_;
    }
    return changed;
  }

  BitWriter& bits_;
  size_t fields_ = 0;
  size_t changed_ = 0;
};

}  // namespace ext
}  // namespace znet
