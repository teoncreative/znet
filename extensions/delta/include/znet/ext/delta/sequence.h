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
// comparison for sequence numbers that wrap.
//
// a snapshot stream numbers its packets with a small unsigned counter, which
// at 60 Hz wraps every 18 minutes for a uint16. Once it does, plain `<` is
// wrong in the one direction that matters: sequence 0 is newer than 65535, not
// older. Every function here treats the halfway point of the type as the
// horizon -- values within half a period ahead count as newer.
//

#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "znet/compat.h"

namespace znet {
namespace ext {

/** @brief Half of the sequence space: the point past which "ahead" reads as "behind". */
template <typename T>
constexpr T SequenceHalfRange() {
  return static_cast<T>(std::numeric_limits<T>::max() / 2 + 1);
}

/**
 * @brief True when @p a is newer than @p b, accounting for wrap.
 *
 * Only meaningful for values genuinely within half a period of each other,
 * which for a uint16 at 60 Hz is nine minutes of stream. A peer that has been
 * silent longer than that cannot be placed, and the caller should treat its
 * baseline as gone rather than trusting an answer here.
 */
template <typename T>
constexpr bool SequenceGreaterThan(T a, T b) {
  static_assert(std::is_unsigned<T>::value,
                "sequence numbers are unsigned and wrap");
  return ((a > b) && (static_cast<T>(a - b) <= SequenceHalfRange<T>())) ||
         ((a < b) && (static_cast<T>(b - a) > SequenceHalfRange<T>()));
}

/** @brief True when @p a is older than @p b, accounting for wrap. */
template <typename T>
constexpr bool SequenceLessThan(T a, T b) {
  return SequenceGreaterThan(b, a);
}

/**
 * @brief How far @p a is ahead of @p b, negative when behind.
 *
 * The subtraction happens in the unsigned type, where wrap is well defined,
 * and only then is the result given a sign.
 */
template <typename T>
constexpr int64_t SequenceDifference(T a, T b) {
  static_assert(std::is_unsigned<T>::value,
                "sequence numbers are unsigned and wrap");
  return static_cast<T>(a - b) >= SequenceHalfRange<T>()
             ? -static_cast<int64_t>(static_cast<T>(b - a))
             : static_cast<int64_t>(static_cast<T>(a - b));
}

}  // namespace ext
}  // namespace znet
