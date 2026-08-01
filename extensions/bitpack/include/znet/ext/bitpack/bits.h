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
// shared vocabulary for the bit packing extension: the width of a field, and
// the bit order both ends of the stream have to agree on.
//

#pragma once

#include <cstdint>

#include "znet/compat.h"
#include "znet/ext/quantize/quantize.h"

namespace znet {
namespace ext {

//
// bit order.
//
// bits fill least-significant-first within each byte, and bytes reach the
// Buffer in order. Writing the 3-bit value 0b101 and then the 2-bit value 0b11
// produces one byte 0b00011101: the first field occupies bits 0-2, the second
// bits 3-4, and the unused high bits are zero.
//
// this is the convention Buffer::WriteBitset already uses. It is unrelated to
// the buffer's endianness setting, which orders bytes within one multi-byte
// number: a bit stream reaches the buffer as a byte stream, so the two never
// interact.
//

namespace detail {

/** @brief Bits needed to represent every value in [0, span]. */
constexpr unsigned BitsForSpan(uint64_t span) {
  // C++14 relaxed constexpr, so this folds for literal bounds instead of
  // looping at runtime for every field.
  unsigned bits = 0;
  while (span > 0) {
    ++bits;
    span >>= 1;
  }
  return bits;
}

// bit-width spellings of znet-quantize's arithmetic. BitWriter, BitReader and
// the delta codec, which compares codes rather than floats, all have to agree
// on which code a value maps to, so they share one implementation rather than
// each rounding for themselves.

/** @brief Distinct codes a @p bits wide field holds, minus one. */
inline double QuantLevels(unsigned bits) {
  return static_cast<double>(quant::LevelsForBits(bits));
}

/** @brief Maps @p value in [min, max] onto a @p bits wide code. */
inline uint32_t QuantizeFloat(float value, float min, float max,
                              unsigned bits) {
  return static_cast<uint32_t>(
      quant::QuantizeToLevels(value, min, max, quant::LevelsForBits(bits)));
}

/** @brief Inverse of QuantizeFloat for the same bounds and width. */
inline float DequantizeFloat(uint32_t code, float min, float max,
                             unsigned bits) {
  return quant::DequantizeFromLevels(code, min, max,
                                     quant::LevelsForBits(bits));
}

}  // namespace detail

/**
 * @brief Widest field a single WriteBits/ReadBits call handles.
 *
 * The scratch register is 64 bits and drains whenever it holds 32 or more, so
 * up to 32 fresh bits always fit alongside whatever is pending. Wider values
 * go through WriteBits64/ReadBits64, which split into two calls.
 */
ZNET_INLINE_CONSTEXPR unsigned kMaxBitsPerCall = 32;

/**
 * @brief Bits needed to distinguish every value in [@p min, @p max].
 *
 * Returns 0 when the range holds a single value: a field that can only be one
 * thing carries no information and costs nothing on the wire.
 *
 * `BitsForRange(0, 1000)` is the compile-time constant 10.
 */
constexpr unsigned BitsForRange(uint64_t min, uint64_t max) {
  return max <= min ? 0u : detail::BitsForSpan(max - min);
}

/**
 * @brief BitsForRange for a signed range.
 *
 * The span is computed in unsigned arithmetic so a range spanning zero, or the
 * whole of int64_t, cannot overflow on the way to its own width.
 */
constexpr unsigned BitsForSignedRange(int64_t min, int64_t max) {
  return max <= min ? 0u
                    : detail::BitsForSpan(static_cast<uint64_t>(max) -
                                          static_cast<uint64_t>(min));
}

}  // namespace ext
}  // namespace znet
