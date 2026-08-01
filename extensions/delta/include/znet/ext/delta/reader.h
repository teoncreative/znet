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
 * @brief Rebuilds a snapshot from a baseline plus the fields that changed.
 *
 * The mirror of DeltaWriter: same calls, same order, same bounds, and the same
 * baseline the sender used.
 *
 * @code
 *   BitReader bits(*buffer);
 *   DeltaReader delta(bits);
 *   now.ammo   = delta.ReadUIntRanged(was.ammo,   0, 200);
 *   now.health = delta.ReadIntRanged (was.health, -50, 100);
 *   now.yaw    = delta.ReadFloatRanged(was.yaw,  -3.15f, 3.15f, 12);
 *   now.firing = delta.ReadBool();
 *   if (!bits.ok()) return nullptr;
 * @endcode
 *
 * An unchanged field returns the baseline clamped to the bounds, which is what
 * the sender compared against, so a baseline that has drifted out of range
 * cannot make the two ends disagree.
 */
class DeltaReader {
 public:
  explicit DeltaReader(BitReader& bits) : bits_(bits) {}

  DeltaReader(const DeltaReader&) = delete;
  DeltaReader& operator=(const DeltaReader&) = delete;

  /** @brief Reads a bool, which DeltaWriter always sends in full. */
  bool ReadBool() {
    ++fields_;
    ++changed_;
    return bits_.ReadBool();
  }

  /** @brief Returns the new value, or @p baseline when the field did not change. */
  uint64_t ReadUIntRanged(uint64_t baseline, uint64_t min, uint64_t max) {
    if (!Flag()) {
      return compat::Clamp(baseline, min, max);
    }
    return bits_.ReadUIntRanged(min, max);
  }

  /** @brief ReadUIntRanged for a signed range. */
  int64_t ReadIntRanged(int64_t baseline, int64_t min, int64_t max) {
    if (!Flag()) {
      return compat::Clamp(baseline, min, max);
    }
    return bits_.ReadIntRanged(min, max);
  }

  /**
   * @brief Returns the new value, or @p baseline requantised, when unchanged.
   *
   * Requantising the baseline rather than passing it through is what keeps a
   * long run of unchanged ticks from drifting: the value a caller feeds back
   * in is one this function returned, so it is already on a quantisation
   * boundary and the round trip is a fixed point.
   */
  float ReadFloatRanged(float baseline, float min, float max, unsigned bits) {
    if (!Flag()) {
      return detail::DequantizeFloat(
          detail::QuantizeFloat(baseline, min, max, bits), min, max, bits);
    }
    return detail::DequantizeFloat(bits_.ReadBits(bits), min, max, bits);
  }

  /** @brief Reads an opaque fixed-width field written by DeltaWriter::WriteBits. */
  uint32_t ReadBits(uint32_t baseline, unsigned bits) {
    if (!Flag()) {
      return baseline;
    }
    return bits_.ReadBits(bits);
  }

  /** @brief Fields read so far. */
  ZNET_NODISCARD size_t fields_read() const { return fields_; }

  /** @brief How many of them carried a value. */
  ZNET_NODISCARD size_t fields_changed() const { return changed_; }

 private:
  bool Flag() {
    ++fields_;
    const bool changed = bits_.ReadBool();
    if (changed) {
      ++changed_;
    }
    return changed;
  }

  BitReader& bits_;
  size_t fields_ = 0;
  size_t changed_ = 0;
};

}  // namespace ext
}  // namespace znet
