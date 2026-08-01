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
// unit-vector compression, on plain floats.
//

#pragma once

#include <cmath>
#include <cstdint>

#include "znet/ext/quantize/scalar.h"

namespace znet {
namespace ext {
namespace quant {

/** @brief Three floats, with no library's layout implied. */
struct Vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

/** @brief Two floats. */
struct Vec2f {
  float u = 0.0f;
  float v = 0.0f;
};

namespace detail {

/** @brief Returns +1 for zero and positives, -1 for negatives. */
inline float NonZeroSign(float value) { return value >= 0.0f ? 1.0f : -1.0f; }

}  // namespace detail

/**
 * @brief Folds a unit vector onto the [-1, 1] square.
 *
 * The sphere is projected onto an octahedron, then the lower half is folded
 * outwards into the corners of the square. Unlike storing x and y and
 * recovering z, this wastes no codes and has no precision collapse near the
 * equator; unlike spherical angles it needs no trigonometry at either end.
 */
inline Vec2f OctEncode(const Vec3f& normal) {
  const float l1 =
      std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z);
  if (!(l1 > 0.0f)) {
    return Vec2f();  // decodes to +Z
  }
  const float x = normal.x / l1;
  const float y = normal.y / l1;
  const float z = normal.z / l1;
  Vec2f out;
  if (z >= 0.0f) {
    out.u = x;
    out.v = y;
  } else {
    out.u = (1.0f - std::fabs(y)) * detail::NonZeroSign(x);
    out.v = (1.0f - std::fabs(x)) * detail::NonZeroSign(y);
  }
  return out;
}

/** @brief Inverse of OctEncode; the result is renormalised. */
inline Vec3f OctDecode(const Vec2f& encoded) {
  float x = encoded.u;
  float y = encoded.v;
  const float z = 1.0f - std::fabs(encoded.u) - std::fabs(encoded.v);
  if (z < 0.0f) {
    const float old_x = x;
    x = (1.0f - std::fabs(y)) * detail::NonZeroSign(old_x);
    y = (1.0f - std::fabs(old_x)) * detail::NonZeroSign(y);
  }
  const float length = std::sqrt(x * x + y * y + z * z);
  if (!(length > 0.0f)) {
    Vec3f fallback;
    fallback.z = 1.0f;
    return fallback;
  }
  Vec3f out;
  out.x = x / length;
  out.y = y / length;
  out.z = z / length;
  return out;
}

/**
 * @brief Packs a direction into two @p bits wide fields, low field first.
 *
 * 16 bits each is 4 bytes at a measured worst case of 0.0037 degrees, which is
 * indistinguishable from three exact floats for shading. 8 bits each is 2
 * bytes at 0.94 degrees, fine for a movement direction or a hit normal.
 *
 * The input need not be normalised but must be non-zero; a zero vector encodes
 * as +Z.
 */
inline uint32_t PackDirectionOct(const Vec3f& value, unsigned bits) {
  const Vec2f encoded = OctEncode(value);
  const uint64_t levels = LevelsForBits(bits);
  const uint64_t u = QuantizeToLevels(encoded.u, -1.0f, 1.0f, levels);
  const uint64_t v = QuantizeToLevels(encoded.v, -1.0f, 1.0f, levels);
  return static_cast<uint32_t>(u) |
         (static_cast<uint32_t>(v) << bits);
}

/** @brief Unpacks a direction written by PackDirectionOct at the same width. */
inline Vec3f UnpackDirectionOct(uint32_t packed, unsigned bits) {
  const uint64_t levels = LevelsForBits(bits);
  const uint32_t mask = static_cast<uint32_t>(levels);
  Vec2f encoded;
  encoded.u = DequantizeFromLevels(packed & mask, -1.0f, 1.0f, levels);
  encoded.v =
      DequantizeFromLevels((packed >> bits) & mask, -1.0f, 1.0f, levels);
  return OctDecode(encoded);
}

}  // namespace quant
}  // namespace ext
}  // namespace znet
