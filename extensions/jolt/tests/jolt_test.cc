//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "znet/ext/jolt/jolt.h"

using znet::Buffer;
using znet::Endianness;
using znet::ext::BitReader;
using znet::ext::BitWriter;
using znet::ext::jolt::BodyQuantization;

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

float QuatLength(const JPH::Quat& q) {
  const float x = q.GetX();
  const float y = q.GetY();
  const float z = q.GetZ();
  const float w = q.GetW();
  return std::sqrt(x * x + y * y + z * z + w * w);
}

/** @brief Shortest angle in degrees between two orientations. */
float RotationAngleDegrees(const JPH::Quat& a, const JPH::Quat& b) {
  const float ax = a.GetX(), ay = a.GetY(), az = a.GetZ(), aw = a.GetW();
  const float bx = b.GetX(), by = b.GetY(), bz = b.GetZ(), bw = b.GetW();
  const float dot = ax * bx + ay * by + az * bz + aw * bw;
  const float s = dot < 0.0f ? -1.0f : 1.0f;
  const float dx = ax - bx * s, dy = ay - by * s;
  const float dz = az - bz * s, dw = aw - bw * s;
  const float sx = ax + bx * s, sy = ay + by * s;
  const float sz = az + bz * s, sw = aw + bw * s;
  return 2.0f *
         std::atan2(std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw),
                    std::sqrt(sx * sx + sy * sy + sz * sz + sw * sw)) *
         57.29577951308232f;
}

}  // namespace

namespace ext = znet::ext::jolt;

// a transform is 80 bits, against 28 bytes for three floats plus four.
TEST(Jolt, TransformIsTenBytes) {
  const BodyQuantization q;
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    ext::WriteTransform(bits, JPH::Vec3(1.0f, 2.0f, 3.0f),
                        JPH::Quat(0.0f, 0.0f, 0.0f, 1.0f), q);
    EXPECT_EQ(bits.bits_written(), 16u * 3u + 32u);
  }
  EXPECT_EQ(buffer->size(), 10u);
}

TEST(Jolt, TransformRoundTrips) {
  const BodyQuantization q;
  const float step = (q.position_max - q.position_min) /
                     static_cast<float>((1u << q.position_bits) - 1u);

  std::mt19937 rng(2026u);
  std::uniform_real_distribution<float> place(q.position_min, q.position_max);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);

  for (int i = 0; i < 5000; ++i) {
    const JPH::Vec3 position(place(rng), place(rng), place(rng));
    const float u1 = unit(rng);
    const float u2 = unit(rng) * 6.2831853f;
    const float u3 = unit(rng) * 6.2831853f;
    const float s1 = std::sqrt(1.0f - u1);
    const float s2 = std::sqrt(u1);
    const JPH::Quat orientation(s1 * std::sin(u2), s1 * std::cos(u2),
                             s2 * std::sin(u3), s2 * std::cos(u3));

    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      ext::WriteTransform(bits, position, orientation, q);
    }
    BitReader bits(*buffer);
    JPH::Vec3 read_position;
    JPH::Quat read_orientation;
    ext::ReadTransform(bits, read_position, read_orientation, q);

    ASSERT_NEAR(read_position.GetX(), position.GetX(), step);
    ASSERT_NEAR(read_position.GetY(), position.GetY(), step);
    ASSERT_NEAR(read_position.GetZ(), position.GetZ(), step);
    ASSERT_LT(RotationAngleDegrees(read_orientation, orientation), 0.15f);
  }
}

// a body at rest must not jitter, so the identity has to survive exactly.
TEST(Jolt, IdentityOrientationIsExact) {
  auto buffer = MakeBuffer();
  const JPH::Quat identity(0.0f, 0.0f, 0.0f, 1.0f);
  {
    BitWriter bits(*buffer);
    ext::WriteOrientation(bits, identity);
  }
  BitReader bits(*buffer);
  EXPECT_EQ(RotationAngleDegrees(ext::ReadOrientation(bits), identity), 0.0f);
}

// a physics engine assumes its quaternions are unit length; a denormalised one
// skews every transform it touches. Whatever arrives, the decode must be unit.
TEST(Jolt, AnyBitsDecodeToAUnitQuaternion) {
  std::mt19937 rng(4242u);
  for (int i = 0; i < 20000; ++i) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteBits(static_cast<uint32_t>(rng() & 0xFFFFFFFFu), 32);
    }
    BitReader bits(*buffer);
    ASSERT_NEAR(QuatLength(ext::ReadOrientation(bits)), 1.0f, 1e-3f);
  }
}

TEST(Jolt, UnnormalisedOrientationIsNormalised) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    ext::WriteOrientation(bits, JPH::Quat(0.0f, 0.0f, 0.0f, 5.0f));
  }
  BitReader bits(*buffer);
  EXPECT_NEAR(QuatLength(ext::ReadOrientation(bits)), 1.0f, 1e-5f);
}

TEST(Jolt, PositionsOutsideTheWorldClamp) {
  const BodyQuantization q;
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    ext::WritePosition(bits, JPH::Vec3(-99999.0f, 99999.0f, 0.0f), q);
  }
  BitReader bits(*buffer);
  const JPH::Vec3 read = ext::ReadPosition(bits, q);
  EXPECT_FLOAT_EQ(read.GetX(), q.position_min);
  EXPECT_FLOAT_EQ(read.GetY(), q.position_max);
}

TEST(Jolt, VelocityRoundTrips) {
  const BodyQuantization q;
  const float linear_step =
      2.0f * q.linear_speed_max /
      static_cast<float>((1u << q.linear_speed_bits) - 1u);

  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    ext::WriteLinearVelocity(bits, JPH::Vec3(12.5f, -60.0f, 3.0f), q);
    ext::WriteAngularVelocity(bits, JPH::Vec3(1.0f, -2.0f, 0.5f), q);
    EXPECT_EQ(bits.bits_written(), 12u * 3u + 10u * 3u);
  }
  BitReader bits(*buffer);
  const JPH::Vec3 linear = ext::ReadLinearVelocity(bits, q);
  const JPH::Vec3 angular = ext::ReadAngularVelocity(bits, q);
  EXPECT_NEAR(linear.GetX(), 12.5f, linear_step);
  EXPECT_NEAR(linear.GetY(), -60.0f, linear_step);
  const float angular_step =
      2.0f * q.angular_speed_max /
      static_cast<float>((1u << q.angular_speed_bits) - 1u);
  EXPECT_NEAR(angular.GetX(), 1.0f, angular_step);
}

// the headline: full body state in 15 bytes against 40 raw.
TEST(Jolt, FullBodyStateIsFifteenBytes) {
  const BodyQuantization q;
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    ext::WriteTransform(bits, JPH::Vec3(1.0f, 2.0f, 3.0f),
                        JPH::Quat(0.0f, 0.0f, 0.0f, 1.0f), q);
    ext::WriteLinearVelocity(bits, JPH::Vec3(1.0f, 0.0f, 0.0f), q);
    EXPECT_EQ(bits.bits_written(), 16u * 3u + 32u + 12u * 3u);
  }
  EXPECT_EQ(buffer->size(), 15u);  // 116 bits rounds up to 15 bytes
  // the same state as raw floats: 3 position + 4 orientation + 3 velocity
  EXPECT_EQ(10u * sizeof(float), 40u);
}
