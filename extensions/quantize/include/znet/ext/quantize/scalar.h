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
// scalar quantisation, on plain floats and integers.
//
// nothing here knows about a Buffer or about anybody's vector type. That is
// the point: the same arithmetic backs znet-glm, znet-bitpack and every
// physics adapter, and those must agree bit for bit or a value written by one
// and read by another comes back wrong. One implementation is the only way to
// guarantee that.
//

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

#include "znet/compat.h"

namespace znet {
namespace ext {
namespace quant {

// ---------------------------------------------------------------------------
// Fixed point over a known range
// ---------------------------------------------------------------------------

/**
 * @brief Maps @p value in [min, max] onto an integer code in [0, levels].
 *
 * Values outside the range clamp to the endpoints; they do not wrap. A range
 * that is empty or inverted yields 0.
 *
 * The arithmetic runs in double deliberately. A 32-bit code has more distinct
 * values than a float has mantissa bits, so computing the scale in float would
 * leave whole swathes of codes unreachable.
 */
inline uint64_t QuantizeToLevels(float value, float min, float max,
                                 uint64_t levels) {
  const double span = static_cast<double>(max) - static_cast<double>(min);
  if (!(span > 0.0) || levels == 0) {
    return 0;
  }
  const double t = compat::Clamp(
      (static_cast<double>(value) - static_cast<double>(min)) / span, 0.0, 1.0);
  return static_cast<uint64_t>(t * static_cast<double>(levels) + 0.5);
}

/** @brief Inverse of QuantizeToLevels for the same bounds and level count. */
inline float DequantizeFromLevels(uint64_t code, float min, float max,
                                  uint64_t levels) {
  if (levels == 0) {
    return min;
  }
  const double span = static_cast<double>(max) - static_cast<double>(min);
  return static_cast<float>(static_cast<double>(min) +
                            span * (static_cast<double>(code) /
                                    static_cast<double>(levels)));
}

/** @brief Distinct codes a @p bits wide field holds, minus one. */
inline uint64_t LevelsForBits(unsigned bits) {
  if (bits == 0) {
    return 0;
  }
  if (bits >= 64) {
    return std::numeric_limits<uint64_t>::max();
  }
  return (UINT64_C(1) << bits) - UINT64_C(1);
}

/** @brief QuantizeToLevels onto the full range of an unsigned integer type. */
template <typename UInt>
UInt QuantizeToInt(float value, float min, float max) {
  return static_cast<UInt>(QuantizeToLevels(
      value, min, max,
      static_cast<uint64_t>(std::numeric_limits<UInt>::max())));
}

/** @brief Inverse of QuantizeToInt. */
template <typename UInt>
float DequantizeFromInt(UInt code, float min, float max) {
  return DequantizeFromLevels(
      static_cast<uint64_t>(code), min, max,
      static_cast<uint64_t>(std::numeric_limits<UInt>::max()));
}

// ---------------------------------------------------------------------------
// Half precision
// ---------------------------------------------------------------------------

/**
 * @brief Converts @p value to IEEE 754 binary16.
 *
 * Written out rather than delegated so that the physics adapters do not have
 * to pull in glm for it. Rounds to nearest, ties to even, which is what the
 * hardware instruction does; handles subnormals, and saturates to infinity
 * rather than wrapping on overflow.
 */
inline uint16_t PackHalf(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));

  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t biased = (bits >> 23) & 0xFFu;
  const uint32_t mantissa = bits & 0x7FFFFFu;

  if (biased == 0xFFu) {
    // infinity, or a NaN whose payload must not be allowed to become infinity
    return static_cast<uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
  }

  const int32_t exponent = static_cast<int32_t>(biased) - 127 + 15;

  if (exponent >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00u);  // saturate to infinity
  }

  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<uint16_t>(sign);  // underflows even a subnormal
    }
    // renormalise into a binary16 subnormal, rounding to nearest even
    const uint32_t full = mantissa | 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    const uint32_t result = full >> shift;
    const uint32_t remainder = full & ((UINT32_C(1) << shift) - 1);
    const uint32_t halfway = UINT32_C(1) << (shift - 1);
    const uint32_t round =
        (remainder > halfway || (remainder == halfway && (result & 1u) != 0))
            ? 1u
            : 0u;
    return static_cast<uint16_t>(sign | (result + round));
  }

  const uint32_t result =
      (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
  const uint32_t remainder = mantissa & 0x1FFFu;
  const uint32_t round =
      (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0)) ? 1u
                                                                            : 0u;
  // a carry out of the mantissa lands in the exponent, which is the correct
  // answer rather than an overflow to fix up
  return static_cast<uint16_t>(sign | (result + round));
}

/** @brief Converts an IEEE 754 binary16 back to a float. Exact in that direction. */
inline float UnpackHalf(uint16_t value) {
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
  const uint32_t exponent = (value >> 10) & 0x1Fu;
  const uint32_t mantissa = value & 0x3FFu;

  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;  // signed zero
    } else {
      // subnormal: renormalise into a float32, which has the exponent range to
      // hold it as a normal number. A binary16 subnormal is mantissa * 2^-24,
      // and shifting left until bit 10 is set makes the leading 1 implicit,
      // leaving 1.f * 2^(-14 - shift).
      uint32_t shift = 0;
      uint32_t m = mantissa;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        ++shift;
      }
      m &= 0x3FFu;
      const uint32_t e = 127u - 14u - shift;
      bits = sign | (e << 23) | (m << 13);
    }
  } else if (exponent == 0x1Fu) {
    bits = sign | 0x7F800000u | (mantissa << 13);  // infinity or NaN
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }

  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

}  // namespace quant
}  // namespace ext
}  // namespace znet
