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
// Box2D body state on the wire.
//
//   #include "znet/ext/box2d/box2d.h"
//
// a planar body is much cheaper to replicate than a 3D one, and not just
// because it has one fewer axis: its orientation is a single angle rather than
// a quaternion, so it costs 10 bits instead of 32. A full transform fits in
// five bytes.
//
// the arithmetic is znet-quantize's, so a position written here and one
// written through znet-glm produce the same codes.
//

#pragma once

#include <box2d/math_functions.h>

#include <cstdint>

#include "znet/compat.h"
#include "znet/ext/bitpack/bitpack.h"
#include "znet/ext/quantize/quantize.h"

namespace znet {
namespace ext {

/**
 * @brief How much precision a planar body's state is worth.
 *
 * Both ends must agree on these. They are not sent, because sending them every
 * tick would cost more than the fields they describe.
 *
 * The defaults suit a world a couple of thousand units across: position lands
 * within 3 cm, and rotation within a fifth of a degree, which is finer than a
 * remote body interpolating at 60 Hz can show.
 */
struct Body2DQuantization {
  /** @brief World extent on each axis. Positions outside it clamp. */
  float position_min = -1024.0f;
  float position_max = 1024.0f;
  unsigned position_bits = 16;

  /** @brief Rotation resolution. 10 bits is 0.35 degrees. */
  unsigned rotation_bits = 10;

  /** @brief Symmetric speed bound, per axis. */
  float linear_speed_max = 256.0f;
  unsigned linear_speed_bits = 12;

  /** @brief Symmetric angular speed bound, in radians per second. */
  float angular_speed_max = 64.0f;
  unsigned angular_speed_bits = 10;
};

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

/** @brief Writes a point, two fields of position_bits. */
inline void WriteVec2(BitWriter& bits, const b2Vec2& value,
                      const Body2DQuantization& q) {
  bits.WriteFloatRanged(value.x, q.position_min, q.position_max,
                        q.position_bits);
  bits.WriteFloatRanged(value.y, q.position_min, q.position_max,
                        q.position_bits);
}

/** @brief Reads a point written by WriteVec2. */
inline b2Vec2 ReadVec2(BitReader& bits, const Body2DQuantization& q) {
  b2Vec2 out;
  out.x = bits.ReadFloatRanged(q.position_min, q.position_max, q.position_bits);
  out.y = bits.ReadFloatRanged(q.position_min, q.position_max, q.position_bits);
  return out;
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------

/**
 * @brief Writes a rotation as a quantised angle.
 *
 * b2Rot is a cosine and a sine, which is two floats for one degree of freedom.
 * Sending the angle instead costs a third as much and cannot arrive
 * denormalised, since any code at all decodes to a unit b2Rot.
 *
 * The angle wraps, so there is no seam: a body spinning past pi does not jump
 * a quantisation step.
 */
inline void WriteRot(BitWriter& bits, const b2Rot& value,
                     const Body2DQuantization& q) {
  bits.WriteBits(quant::PackAngle(b2Rot_GetAngle(value), q.rotation_bits),
                 q.rotation_bits);
}

/** @brief Reads a rotation written by WriteRot. Always unit length. */
inline b2Rot ReadRot(BitReader& bits, const Body2DQuantization& q) {
  return b2MakeRot(
      quant::UnpackAngle(bits.ReadBits(q.rotation_bits), q.rotation_bits));
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

/** @brief Writes position and rotation: 42 bits at the defaults. */
inline void WriteTransform(BitWriter& bits, const b2Transform& value,
                           const Body2DQuantization& q) {
  WriteVec2(bits, value.p, q);
  WriteRot(bits, value.q, q);
}

/** @brief Reads a transform written by WriteTransform. */
inline b2Transform ReadTransform(BitReader& bits,
                                 const Body2DQuantization& q) {
  b2Transform out;
  out.p = ReadVec2(bits, q);
  out.q = ReadRot(bits, q);
  return out;
}

// ---------------------------------------------------------------------------
// Velocity
// ---------------------------------------------------------------------------

/** @brief Writes a linear velocity over the configured speed bound. */
inline void WriteLinearVelocity(BitWriter& bits, const b2Vec2& value,
                                const Body2DQuantization& q) {
  bits.WriteFloatRanged(value.x, -q.linear_speed_max, q.linear_speed_max,
                        q.linear_speed_bits);
  bits.WriteFloatRanged(value.y, -q.linear_speed_max, q.linear_speed_max,
                        q.linear_speed_bits);
}

/** @brief Reads a linear velocity written by WriteLinearVelocity. */
inline b2Vec2 ReadLinearVelocity(BitReader& bits,
                                 const Body2DQuantization& q) {
  b2Vec2 out;
  out.x = bits.ReadFloatRanged(-q.linear_speed_max, q.linear_speed_max,
                               q.linear_speed_bits);
  out.y = bits.ReadFloatRanged(-q.linear_speed_max, q.linear_speed_max,
                               q.linear_speed_bits);
  return out;
}

/** @brief Writes an angular velocity in radians per second. */
inline void WriteAngularVelocity(BitWriter& bits, float value,
                                 const Body2DQuantization& q) {
  bits.WriteFloatRanged(value, -q.angular_speed_max, q.angular_speed_max,
                        q.angular_speed_bits);
}

/** @brief Reads an angular velocity written by WriteAngularVelocity. */
inline float ReadAngularVelocity(BitReader& bits,
                                 const Body2DQuantization& q) {
  return bits.ReadFloatRanged(-q.angular_speed_max, q.angular_speed_max,
                              q.angular_speed_bits);
}

}  // namespace ext
}  // namespace znet
