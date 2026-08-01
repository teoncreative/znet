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
// orientation compression, on plain floats.
//

#pragma once

#include <cmath>
#include <cstdint>

#include "znet/compat.h"
#include "znet/ext/quantize/scalar.h"

namespace znet {
namespace ext {
namespace quant {

/** @brief Four floats in x, y, z, w order, with no library's layout implied. */
struct Quat4 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

namespace detail {

/** @brief 1/sqrt(2): the largest a non-largest component of a unit quaternion can be. */
const float kInvSqrt2 = 0.7071067811865475f;
const float kSqrt2 = 1.4142135623730951f;

/** @brief Field width: 10 bits per stored component. */
const uint32_t kSmallestThreeMask = 0x3FFu;

/**
 * @brief Highest code a component may take: 1022, so codes run 0..1022.
 *
 * That is 1023 levels rather than the 1024 the field could hold, and the odd
 * count is the point. An even number of levels puts zero halfway between two
 * codes, so the identity quaternion, whose three stored components are all
 * exactly zero, would come back as a rotation of 0.137 degrees and a resting
 * object would jitter for free. Identity is the single most common rotation on
 * the wire, so it is the wrong thing to be inexact about. An odd count puts a
 * code exactly on zero; the price is one unused code and a step 0.1% larger.
 */
const uint32_t kSmallestThreeMaxCode = 1022u;

}  // namespace detail

/**
 * @brief Packs a unit quaternion into 32 bits.
 *
 * Only three components are sent. A unit quaternion satisfies
 * x^2+y^2+z^2+w^2 = 1, so the fourth is recoverable; dropping the *largest*
 * one is what makes it safe, because the recovered component is then at least
 * 1/2 and the square root is never near-singular. Since q and -q are the same
 * rotation, the quaternion is negated when needed so the dropped component is
 * positive and its sign does not have to travel.
 *
 * Layout, most significant bit first: 2 bits naming the dropped component
 * (0=x, 1=y, 2=z, 3=w), then the other three at 10 bits each in ascending
 * component order, each mapped from [-1/sqrt(2), 1/sqrt(2)].
 *
 * Worst-case error measured over 300k uniformly sampled rotations is 0.115
 * degrees, and the identity round-trips bit-exactly. The input is normalised
 * first; a zero quaternion packs as the identity.
 */
inline uint32_t PackQuatSmallestThree(const Quat4& value) {
  Quat4 q = value;
  const float length_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (!(length_sq > 0.0f)) {
    q = Quat4();
  } else {
    const float inv_length = 1.0f / std::sqrt(length_sq);
    q.x *= inv_length;
    q.y *= inv_length;
    q.z *= inv_length;
    q.w *= inv_length;
  }

  const float components[4] = {q.x, q.y, q.z, q.w};
  int largest = 0;
  for (int i = 1; i < 4; ++i) {
    if (std::fabs(components[i]) > std::fabs(components[largest])) {
      largest = i;
    }
  }
  const float sign = components[largest] < 0.0f ? -1.0f : 1.0f;

  uint32_t packed = static_cast<uint32_t>(largest) << 30;
  int shift = 20;
  for (int i = 0; i < 4; ++i) {
    if (i == largest) {
      continue;
    }
    // [-1/sqrt(2), 1/sqrt(2)] -> [-1, 1] -> [0, 1]
    const float t = compat::Clamp(
        (components[i] * sign * detail::kSqrt2 + 1.0f) * 0.5f, 0.0f, 1.0f);
    const uint32_t code = static_cast<uint32_t>(
        t * static_cast<float>(detail::kSmallestThreeMaxCode) + 0.5f);
    packed |= (code & detail::kSmallestThreeMask) << shift;
    shift -= 10;
  }
  return packed;
}

/** @brief Unpacks a quaternion written by PackQuatSmallestThree. */
inline Quat4 UnpackQuatSmallestThree(uint32_t packed) {
  const uint32_t largest = (packed >> 30) & 0x3u;
  float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float sum_sq = 0.0f;
  int shift = 20;
  for (uint32_t i = 0; i < 4; ++i) {
    if (i == largest) {
      continue;
    }
    // an encoder never emits the one unused code, but this word may not have
    // come from one
    uint32_t code = (packed >> shift) & detail::kSmallestThreeMask;
    if (code > detail::kSmallestThreeMaxCode) {
      code = detail::kSmallestThreeMaxCode;
    }
    const float t = static_cast<float>(code) /
                    static_cast<float>(detail::kSmallestThreeMaxCode);
    const float component = (t * 2.0f - 1.0f) * detail::kInvSqrt2;
    components[i] = component;
    sum_sq += component * component;
    shift -= 10;
  }
  // the dropped component was the largest, so for a word from PackQuat* this
  // is >= 1/2 and sum_sq is at most 3/4. An arbitrary word can put sum_sq as
  // high as 3/2, which the max() turns into a zero here and the normalisation
  // below turns into a unit quaternion: a caller must be able to feed rotation
  // straight from a decoded packet into a matrix without checking it first.
  components[largest] = std::sqrt(std::fmax(0.0f, 1.0f - sum_sq));

  const float length = std::sqrt(components[0] * components[0] +
                                 components[1] * components[1] +
                                 components[2] * components[2] +
                                 components[3] * components[3]);
  if (!(length > 0.0f)) {
    return Quat4();
  }
  const float inv_length = 1.0f / length;
  Quat4 out;
  out.x = components[0] * inv_length;
  out.y = components[1] * inv_length;
  out.z = components[2] * inv_length;
  out.w = components[3] * inv_length;
  return out;
}

/**
 * @brief Packs a 2D orientation into @p bits, wrapping at the seam.
 *
 * A planar body's orientation is one angle, not a quaternion, so it costs a
 * fraction as much. The range is treated as a circle: the code for +pi and the
 * code for -pi are the same, so there is no seam where a body spinning through
 * the wrap jumps a whole step.
 */
inline uint32_t PackAngle(float radians, unsigned bits) {
  const uint64_t levels = LevelsForBits(bits) + 1;  // wrap: no duplicate endpoint
  if (levels <= 1) {
    return 0;
  }
  const double two_pi = 6.283185307179586;
  double turns = static_cast<double>(radians) / two_pi;
  turns -= std::floor(turns);  // into [0, 1)
  uint64_t code = static_cast<uint64_t>(turns * static_cast<double>(levels) + 0.5);
  if (code >= levels) {
    code = 0;  // rounding up past the top wraps to the bottom
  }
  return static_cast<uint32_t>(code);
}

/** @brief Unpacks an angle written by PackAngle, into [-pi, pi). */
inline float UnpackAngle(uint32_t code, unsigned bits) {
  const uint64_t levels = LevelsForBits(bits) + 1;
  if (levels <= 1) {
    return 0.0f;
  }
  const double two_pi = 6.283185307179586;
  const double turns =
      static_cast<double>(code % levels) / static_cast<double>(levels);
  double radians = turns * two_pi;
  if (radians >= two_pi * 0.5) {
    radians -= two_pi;
  }
  return static_cast<float>(radians);
}

}  // namespace quant
}  // namespace ext
}  // namespace znet
