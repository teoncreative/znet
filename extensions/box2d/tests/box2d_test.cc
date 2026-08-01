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

#include "znet/ext/box2d/box2d.h"

using znet::Buffer;
using znet::Endianness;
using znet::ext::BitReader;
using znet::ext::BitWriter;
using znet::ext::Body2DQuantization;

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

/** @brief Smallest signed difference between two angles. */
float AngleDifference(float a, float b) {
  float d = a - b;
  while (d > 3.14159265f) d -= 6.28318531f;
  while (d < -3.14159265f) d += 6.28318531f;
  return d;
}

}  // namespace

TEST(Box2D, TransformFitsInFiveBytes) {
  const Body2DQuantization q;
  auto buffer = MakeBuffer();

  b2Transform transform;
  transform.p = b2Vec2{12.5f, -30.25f};
  transform.q = b2MakeRot(1.0f);
  {
    BitWriter bits(*buffer);
    znet::ext::WriteTransform(bits, transform, q);
    EXPECT_EQ(bits.bits_written(), 16u + 16u + 10u);
  }
  // 42 bits, against 20 bytes for three raw floats plus a b2Rot
  EXPECT_EQ(buffer->size(), 6u);
}

TEST(Box2D, TransformRoundTrips) {
  const Body2DQuantization q;
  const float position_step =
      (q.position_max - q.position_min) /
      static_cast<float>((1u << q.position_bits) - 1u);
  const float rotation_step = 6.28318531f / static_cast<float>(1u << q.rotation_bits);

  std::mt19937 rng(2026u);
  std::uniform_real_distribution<float> place(q.position_min, q.position_max);
  std::uniform_real_distribution<float> spin(-3.14159265f, 3.14159265f);

  for (int i = 0; i < 5000; ++i) {
    b2Transform written;
    written.p = b2Vec2{place(rng), place(rng)};
    written.q = b2MakeRot(spin(rng));

    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      znet::ext::WriteTransform(bits, written, q);
    }
    BitReader bits(*buffer);
    const b2Transform read = znet::ext::ReadTransform(bits, q);

    ASSERT_NEAR(read.p.x, written.p.x, position_step);
    ASSERT_NEAR(read.p.y, written.p.y, position_step);
    ASSERT_LT(std::fabs(AngleDifference(b2Rot_GetAngle(read.q),
                                        b2Rot_GetAngle(written.q))),
              rotation_step);
  }
}

// a rotation is a circle. A body spinning through pi must not jump a whole
// quantisation step at the seam, which is what happens when the encoding
// treats the angle as an interval with two distinct endpoints.
TEST(Box2D, RotationHasNoSeam) {
  const Body2DQuantization q;
  const float rotation_step = 6.28318531f / static_cast<float>(1u << q.rotation_bits);

  for (int i = -20; i <= 20; ++i) {
    const float angle = 3.14159265f + static_cast<float>(i) * rotation_step * 0.25f;

    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      znet::ext::WriteRot(bits, b2MakeRot(angle), q);
    }
    BitReader bits(*buffer);
    const b2Rot read = znet::ext::ReadRot(bits, q);
    ASSERT_LT(std::fabs(AngleDifference(b2Rot_GetAngle(read), angle)),
              rotation_step)
        << "angle " << angle;
  }
}

// whatever the bits say, a decoded b2Rot must be unit length: Box2D's maths
// assumes it, and a denormalised rotation silently skews every transform it
// touches.
TEST(Box2D, AnyBitsDecodeToAUnitRotation) {
  const Body2DQuantization q;
  std::mt19937 rng(77u);
  for (int i = 0; i < 20000; ++i) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteBits(static_cast<uint32_t>(rng() & 0xFFFFFFFFu), q.rotation_bits);
    }
    BitReader bits(*buffer);
    const b2Rot read = znet::ext::ReadRot(bits, q);
    ASSERT_NEAR(std::sqrt(read.c * read.c + read.s * read.s), 1.0f, 1e-5f);
  }
}

TEST(Box2D, PositionsOutsideTheWorldClamp) {
  const Body2DQuantization q;
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    znet::ext::WriteVec2(bits, b2Vec2{-99999.0f, 99999.0f}, q);
  }
  BitReader bits(*buffer);
  const b2Vec2 read = znet::ext::ReadVec2(bits, q);
  EXPECT_FLOAT_EQ(read.x, q.position_min);
  EXPECT_FLOAT_EQ(read.y, q.position_max);
}

TEST(Box2D, VelocityRoundTrips) {
  const Body2DQuantization q;
  const float linear_step = 2.0f * q.linear_speed_max /
                            static_cast<float>((1u << q.linear_speed_bits) - 1u);
  const float angular_step =
      2.0f * q.angular_speed_max /
      static_cast<float>((1u << q.angular_speed_bits) - 1u);

  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    znet::ext::WriteLinearVelocity(bits, b2Vec2{12.5f, -60.0f}, q);
    znet::ext::WriteAngularVelocity(bits, 3.5f, q);
    EXPECT_EQ(bits.bits_written(), 12u + 12u + 10u);
  }
  BitReader bits(*buffer);
  const b2Vec2 linear = znet::ext::ReadLinearVelocity(bits, q);
  const float angular = znet::ext::ReadAngularVelocity(bits, q);
  EXPECT_NEAR(linear.x, 12.5f, linear_step);
  EXPECT_NEAR(linear.y, -60.0f, linear_step);
  EXPECT_NEAR(angular, 3.5f, angular_step);
}

// the whole reason a planar body is cheap: orientation is one angle rather
// than a quaternion. A full transform plus velocity is 76 bits, against 28
// bytes for the same state as raw floats.
TEST(Box2D, FullBodyStateIsTenBytesInsteadOfTwentyEight) {
  const Body2DQuantization q;
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    b2Transform transform;
    transform.p = b2Vec2{1.0f, 2.0f};
    transform.q = b2MakeRot(0.5f);
    znet::ext::WriteTransform(bits, transform, q);
    znet::ext::WriteLinearVelocity(bits, b2Vec2{3.0f, 4.0f}, q);
    znet::ext::WriteAngularVelocity(bits, 0.25f, q);
    EXPECT_EQ(bits.bits_written(), 16u + 16u + 10u + 12u + 12u + 10u);
  }
  EXPECT_EQ(buffer->size(), 10u);

  const size_t raw = sizeof(b2Transform) + sizeof(b2Vec2) + sizeof(float);
  EXPECT_EQ(raw, 28u);
}

TEST(Box2D, CustomQuantizationIsHonoured) {
  Body2DQuantization q;
  q.position_min = -10.0f;
  q.position_max = 10.0f;
  q.position_bits = 8;
  q.rotation_bits = 6;

  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    b2Transform transform;
    transform.p = b2Vec2{5.0f, -5.0f};
    transform.q = b2MakeRot(0.0f);
    znet::ext::WriteTransform(bits, transform, q);
    EXPECT_EQ(bits.bits_written(), 8u + 8u + 6u);
  }
  EXPECT_EQ(buffer->size(), 3u);
}
