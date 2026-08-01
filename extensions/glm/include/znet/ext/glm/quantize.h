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
// lossy, bandwidth-shaped encodings for the glm types that dominate a game's
// state updates.
//
// the exact writers in serialize.h are the right default. These exist for the
// fields that are sent every tick to every peer, where the full float is
// paying for precision nobody can observe:
//
//   position   vec3   12 B  ->  6 B   half, or 4-6 B ranged over the world box
//   rotation   quat   16 B  ->  4 B   smallest-three
//   normal     vec3   12 B  ->  4 B   octahedral 16:16, or 2 B at 8:8
//
// every function here is self-describing on the wire only insofar as its size
// is fixed: the reader must call the matching Read* for the Write* the sender
// used, exactly as with the exact functions.
//

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/ext/glm/serialize.h"
#include "znet/ext/quantize/quantize.h"

namespace znet {
namespace ext {

// ---------------------------------------------------------------------------
// Half precision
// ---------------------------------------------------------------------------
//
// IEEE 754 binary16: ~3 decimal digits, magnitudes up to 65504. Good for
// velocities, offsets and colours; not for world-space positions on a large
// map, where the exponent buys range at the cost of a resolution that degrades
// the further you are from the origin (at 1024 units the step is already 1
// unit). Use WriteVecRanged for those.

/** @brief Writes @p value as a 2-byte binary16. */
inline void WriteHalf(Buffer& buffer, float value) {
  buffer.WriteInt<uint16_t>(glm::packHalf1x16(value));
}

/** @brief Reads a 2-byte binary16 written by WriteHalf. */
inline float ReadHalf(Buffer& buffer) {
  return glm::unpackHalf1x16(buffer.ReadInt<uint16_t>());
}

/** @brief Writes each component of @p value as a binary16: 2 bytes each. */
template <glm::length_t L, glm::qualifier Q>
void WriteVecHalf(Buffer& buffer, const glm::vec<L, float, Q>& value) {
  for (glm::length_t i = 0; i < L; ++i) {
    WriteHalf(buffer, value[i]);
  }
}

/** @brief Reads a vector written by WriteVecHalf into @p out. */
template <glm::length_t L, glm::qualifier Q>
void ReadVecHalf(Buffer& buffer, glm::vec<L, float, Q>& out) {
  for (glm::length_t i = 0; i < L; ++i) {
    out[i] = ReadHalf(buffer);
  }
}

/** @brief Value-returning form: `auto v = ReadVecHalf<glm::vec3>(buffer);` */
template <typename VecT>
VecT ReadVecHalf(Buffer& buffer) {
  VecT out(0.0f);
  ReadVecHalf(buffer, out);
  return out;
}

// ---------------------------------------------------------------------------
// Fixed-point over a known range
// ---------------------------------------------------------------------------
//
// unlike a half, the step size here is uniform across the whole range, which
// is what you want for a coordinate: 16 bits over a 2 km world is a uniform
// 3 cm everywhere, where a half would give millimetres near the origin and
// two metres at the far corner.

/**
 * @brief Maps @p value from [@p min, @p max] onto the full range of @p UInt.
 *
 * Values outside the range clamp to the endpoints; they do not wrap. The
 * arithmetic runs in double because a uint32_t target has more distinct codes
 * than a float has mantissa bits, and rounding the scale in float would make
 * whole swathes of codes unreachable.
 */
template <typename UInt>
UInt QuantizeFloat(float value, float min, float max) {
  static_assert(std::is_unsigned<UInt>::value,
                "QuantizeFloat stores into an unsigned integer type");
  return quant::QuantizeToInt<UInt>(value, min, max);
}

/** @brief Inverse of QuantizeFloat for the same @p min and @p max. */
template <typename UInt>
float DequantizeFloat(UInt value, float min, float max) {
  static_assert(std::is_unsigned<UInt>::value,
                "DequantizeFloat reads from an unsigned integer type");
  return quant::DequantizeFromInt<UInt>(value, min, max);
}

/**
 * @brief Writes @p value as sizeof(UInt) bytes of fixed point over [min, max].
 *
 * Worst-case error is (max - min) / (2 * UInt_max): for a 16-bit code over a
 * 2000-unit range, 1.5 cm.
 */
template <typename UInt>
void WriteFloatRanged(Buffer& buffer, float value, float min, float max) {
  buffer.WriteInt<UInt>(QuantizeFloat<UInt>(value, min, max));
}

/** @brief Reads a value written by WriteFloatRanged with the same bounds. */
template <typename UInt>
float ReadFloatRanged(Buffer& buffer, float min, float max) {
  return DequantizeFloat<UInt>(buffer.ReadInt<UInt>(), min, max);
}

/** @brief Writes every component of @p value over the same [min, max]. */
template <typename UInt, glm::length_t L, glm::qualifier Q>
void WriteVecRanged(Buffer& buffer, const glm::vec<L, float, Q>& value,
                    float min, float max) {
  for (glm::length_t i = 0; i < L; ++i) {
    WriteFloatRanged<UInt>(buffer, value[i], min, max);
  }
}

/** @brief Reads a vector written by WriteVecRanged into @p out. */
template <typename UInt, glm::length_t L, glm::qualifier Q>
void ReadVecRanged(Buffer& buffer, glm::vec<L, float, Q>& out, float min,
                   float max) {
  for (glm::length_t i = 0; i < L; ++i) {
    out[i] = ReadFloatRanged<UInt>(buffer, min, max);
  }
}

/**
 * @brief Value-returning form.
 *
 * `auto p = ReadVecRanged<uint16_t, glm::vec3>(buffer, -1000.f, 1000.f);`
 */
template <typename UInt, typename VecT>
VecT ReadVecRanged(Buffer& buffer, float min, float max) {
  VecT out(0.0f);
  ReadVecRanged<UInt>(buffer, out, min, max);
  return out;
}

// ---------------------------------------------------------------------------
// Unit quaternions: smallest-three
// ---------------------------------------------------------------------------

/**
 * @brief Packs a unit quaternion into 32 bits.
 *
 * A thin conversion over znet-quantize's implementation, which is where the
 * scheme and its rationale live. Sharing it is not tidiness: the physics
 * adapters pack the same rotations, and two copies that disagreed by one
 * rounding would give a Bullet client and a glm client different orientations
 * from identical bytes.
 *
 * Worst-case error measured over 300k uniformly sampled rotations is 0.115
 * degrees, and the identity round-trips bit-exactly.
 */
inline uint32_t PackQuatSmallestThree(const glm::quat& value) {
  quant::Quat4 neutral;
  neutral.x = value.x;
  neutral.y = value.y;
  neutral.z = value.z;
  neutral.w = value.w;
  return quant::PackQuatSmallestThree(neutral);
}

/** @brief Unpacks a quaternion written by PackQuatSmallestThree. */
inline glm::quat UnpackQuatSmallestThree(uint32_t packed) {
  const quant::Quat4 neutral = quant::UnpackQuatSmallestThree(packed);
  return glm::quat(neutral.w, neutral.x, neutral.y, neutral.z);
}

/** @brief Writes a unit quaternion as 4 bytes. See PackQuatSmallestThree. */
inline void WriteQuatSmallestThree(Buffer& buffer, const glm::quat& value) {
  buffer.WriteInt<uint32_t>(PackQuatSmallestThree(value));
}

/** @brief Reads a quaternion written by WriteQuatSmallestThree. */
inline glm::quat ReadQuatSmallestThree(Buffer& buffer) {
  return UnpackQuatSmallestThree(buffer.ReadInt<uint32_t>());
}

// ---------------------------------------------------------------------------
// Unit vectors: octahedral mapping
// ---------------------------------------------------------------------------

/**
 * @brief Writes a unit vector as sizeof(UInt) * 2 bytes.
 *
 * uint16_t gives 4 bytes at a measured worst case of 0.0037 degrees, which is
 * indistinguishable from the exact 12-byte form for shading. uint8_t gives 2
 * bytes at 0.94 degrees, fine for a movement direction or a hit normal.
 *
 * @p value need not be normalised, but it must be non-zero; a zero vector
 * encodes as +Z.
 */
template <typename UInt>
void WriteNormalOct(Buffer& buffer, const glm::vec3& value) {
  quant::Vec3f neutral;
  neutral.x = value.x;
  neutral.y = value.y;
  neutral.z = value.z;
  const quant::Vec2f encoded = quant::OctEncode(neutral);
  buffer.WriteInt<UInt>(QuantizeFloat<UInt>(encoded.u, -1.0f, 1.0f));
  buffer.WriteInt<UInt>(QuantizeFloat<UInt>(encoded.v, -1.0f, 1.0f));
}

/** @brief Reads a unit vector written by WriteNormalOct with the same UInt. */
template <typename UInt>
glm::vec3 ReadNormalOct(Buffer& buffer) {
  quant::Vec2f encoded;
  encoded.u = DequantizeFloat<UInt>(buffer.ReadInt<UInt>(), -1.0f, 1.0f);
  encoded.v = DequantizeFloat<UInt>(buffer.ReadInt<UInt>(), -1.0f, 1.0f);
  const quant::Vec3f decoded = quant::OctDecode(encoded);
  return glm::vec3(decoded.x, decoded.y, decoded.z);
}

}  // namespace ext
}  // namespace znet
