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
// Bullet rigid body state on the wire.
//
//   #include "znet/ext/bullet/bullet.h"
//
// a 3D body's replicated state is a position, an orientation and usually a
// velocity. Sent raw that is 40 bytes; sent through here it is 15, and the
// orientation is the big win: a unit quaternion carries three degrees of
// freedom, so the fourth component is redundant and the smallest-three
// encoding drops it for 4 bytes instead of 16.
//
// the arithmetic is znet-quantize's, shared with znet-glm and the other
// engines. That matters if a server built against one of them talks to a
// client built against another: identical bytes have to mean identical
// transforms, which two independent copies of the same rounding would not
// guarantee.
//
// everything lives in namespace znet::ext::bullet, because the readers here and
// the ones in a sibling engine's adapter differ only by return type.
//

#pragma once

#include <LinearMath/btQuaternion.h>
#include <LinearMath/btVector3.h>

#include <cstdint>

#include "znet/compat.h"
#include "znet/ext/bitpack/bitpack.h"
#include "znet/ext/quantize/quantize.h"

namespace znet {
namespace ext {
namespace bullet {

/**
 * @brief How much precision a body's state is worth.
 *
 * Both ends must agree on these; they are not sent, because sending them every
 * tick would cost more than the fields they describe. The defaults suit a
 * world about two thousand units across, landing positions within 3 cm.
 */
struct BodyQuantization {
  /** @brief World extent on each axis. Positions outside it clamp. */
  float position_min = -1024.0f;
  float position_max = 1024.0f;
  unsigned position_bits = 16;

  /** @brief Symmetric speed bound, per axis. */
  float linear_speed_max = 256.0f;
  unsigned linear_speed_bits = 12;

  /** @brief Symmetric angular speed bound, in radians per second, per axis. */
  float angular_speed_max = 64.0f;
  unsigned angular_speed_bits = 10;
};

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

/** @brief Writes a position as three fields of position_bits. */
inline void WritePosition(BitWriter& bits, const btVector3& value,
                          const BodyQuantization& q) {
  bits.WriteFloatRanged(static_cast<float>(value.getX()), q.position_min, q.position_max, q.position_bits);
  bits.WriteFloatRanged(static_cast<float>(value.getY()), q.position_min, q.position_max, q.position_bits);
  bits.WriteFloatRanged(static_cast<float>(value.getZ()), q.position_min, q.position_max, q.position_bits);
}

/** @brief Reads a position written by WritePosition. */
inline btVector3 ReadPosition(BitReader& bits, const BodyQuantization& q) {
  const float x =
      bits.ReadFloatRanged(q.position_min, q.position_max, q.position_bits);
  const float y =
      bits.ReadFloatRanged(q.position_min, q.position_max, q.position_bits);
  const float z =
      bits.ReadFloatRanged(q.position_min, q.position_max, q.position_bits);
  return btVector3(x, y, z);
}

// ---------------------------------------------------------------------------
// Orientation
// ---------------------------------------------------------------------------

/**
 * @brief Writes an orientation as 32 bits, smallest-three encoded.
 *
 * Worst-case error is 0.115 degrees and the identity is exact, so a body at
 * rest does not jitter. The input need not be normalised.
 */
inline void WriteOrientation(BitWriter& bits, const btQuaternion& value) {
  quant::Quat4 neutral;
  neutral.x = static_cast<float>(value.getX());
  neutral.y = static_cast<float>(value.getY());
  neutral.z = static_cast<float>(value.getZ());
  neutral.w = static_cast<float>(value.getW());
  bits.WriteBits(quant::PackQuatSmallestThree(neutral), 32);
}

/**
 * @brief Reads an orientation written by WriteOrientation.
 *
 * Always unit length, whatever the bits said. A physics engine's maths assumes
 * that, and a denormalised quaternion skews every transform it touches, so
 * this is checked rather than hoped for.
 */
inline btQuaternion ReadOrientation(BitReader& bits) {
  const quant::Quat4 q = quant::UnpackQuatSmallestThree(bits.ReadBits(32));
  return btQuaternion(q.x, q.y, q.z, q.w);
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

/** @brief Writes position and orientation: 80 bits at the defaults. */
inline void WriteTransform(BitWriter& bits, const btVector3& position,
                           const btQuaternion& orientation,
                           const BodyQuantization& q) {
  WritePosition(bits, position, q);
  WriteOrientation(bits, orientation);
}

/** @brief Reads a transform written by WriteTransform. */
inline void ReadTransform(BitReader& bits, btVector3& position,
                          btQuaternion& orientation, const BodyQuantization& q) {
  position = ReadPosition(bits, q);
  orientation = ReadOrientation(bits);
}

// ---------------------------------------------------------------------------
// Velocity
// ---------------------------------------------------------------------------

/** @brief Writes a linear velocity over the configured speed bound. */
inline void WriteLinearVelocity(BitWriter& bits, const btVector3& value,
                                const BodyQuantization& q) {
  bits.WriteFloatRanged(static_cast<float>(value.getX()), -q.linear_speed_max, q.linear_speed_max,
                        q.linear_speed_bits);
  bits.WriteFloatRanged(static_cast<float>(value.getY()), -q.linear_speed_max, q.linear_speed_max,
                        q.linear_speed_bits);
  bits.WriteFloatRanged(static_cast<float>(value.getZ()), -q.linear_speed_max, q.linear_speed_max,
                        q.linear_speed_bits);
}

/** @brief Reads a linear velocity written by WriteLinearVelocity. */
inline btVector3 ReadLinearVelocity(BitReader& bits, const BodyQuantization& q) {
  const float x = bits.ReadFloatRanged(-q.linear_speed_max, q.linear_speed_max,
                                       q.linear_speed_bits);
  const float y = bits.ReadFloatRanged(-q.linear_speed_max, q.linear_speed_max,
                                       q.linear_speed_bits);
  const float z = bits.ReadFloatRanged(-q.linear_speed_max, q.linear_speed_max,
                                       q.linear_speed_bits);
  return btVector3(x, y, z);
}

/** @brief Writes an angular velocity over the configured bound. */
inline void WriteAngularVelocity(BitWriter& bits, const btVector3& value,
                                 const BodyQuantization& q) {
  bits.WriteFloatRanged(static_cast<float>(value.getX()), -q.angular_speed_max, q.angular_speed_max,
                        q.angular_speed_bits);
  bits.WriteFloatRanged(static_cast<float>(value.getY()), -q.angular_speed_max, q.angular_speed_max,
                        q.angular_speed_bits);
  bits.WriteFloatRanged(static_cast<float>(value.getZ()), -q.angular_speed_max, q.angular_speed_max,
                        q.angular_speed_bits);
}

/** @brief Reads an angular velocity written by WriteAngularVelocity. */
inline btVector3 ReadAngularVelocity(BitReader& bits, const BodyQuantization& q) {
  const float x = bits.ReadFloatRanged(-q.angular_speed_max,
                                       q.angular_speed_max, q.angular_speed_bits);
  const float y = bits.ReadFloatRanged(-q.angular_speed_max,
                                       q.angular_speed_max, q.angular_speed_bits);
  const float z = bits.ReadFloatRanged(-q.angular_speed_max,
                                       q.angular_speed_max, q.angular_speed_bits);
  return btVector3(x, y, z);
}

}  // namespace bullet
}  // namespace ext
}  // namespace znet
