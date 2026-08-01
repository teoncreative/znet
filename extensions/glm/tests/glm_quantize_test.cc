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

#include "znet/ext/glm/quantize.h"

using znet::Buffer;
using znet::Endianness;

namespace ext = znet::ext;

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

const float kRadiansToDegrees = 57.29577951308232f;

// both helpers use 2*atan2(|a-b|, |a+b|) rather than acos(dot).
//
// acos is useless at exactly the accuracy these tests need to measure: near a
// dot product of 1 its derivative is unbounded, so in float32 the smallest
// representable step below 1.0 already comes out as 0.04 degrees. A test that
// asserted a tighter bound than that would be measuring its own metric. The
// atan2 form is well conditioned all the way to zero.

/** @brief Shortest angle in degrees between the rotations two quaternions name. */
float RotationAngleDegrees(const glm::quat& a, const glm::quat& b) {
  const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  // q and -q are the same rotation; compare against whichever is nearer
  const float s = dot < 0.0f ? -1.0f : 1.0f;
  const glm::quat aligned(b.w * s, b.x * s, b.y * s, b.z * s);
  const float difference = glm::length(a - aligned);
  const float sum = glm::length(a + aligned);
  return 2.0f * std::atan2(difference, sum) * kRadiansToDegrees;
}

/** @brief Angle in degrees between two non-zero vectors. */
float AngleBetweenDegrees(const glm::vec3& a, const glm::vec3& b) {
  const glm::vec3 na = glm::normalize(a);
  const glm::vec3 nb = glm::normalize(b);
  return 2.0f * std::atan2(glm::length(na - nb), glm::length(na + nb)) *
         kRadiansToDegrees;
}

/** @brief Deterministic unit quaternions (Shoemake's uniform sampling). */
std::vector<glm::quat> SampleQuats(size_t count) {
  std::mt19937 rng(1337u);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::vector<glm::quat> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const float u1 = unit(rng);
    const float u2 = unit(rng) * 6.2831853f;
    const float u3 = unit(rng) * 6.2831853f;
    const float s1 = std::sqrt(1.0f - u1);
    const float s2 = std::sqrt(u1);
    out.push_back(glm::quat(s2 * std::cos(u3), s1 * std::sin(u2),
                            s1 * std::cos(u2), s2 * std::sin(u3)));
  }
  return out;
}

std::vector<glm::vec3> SampleUnitVectors(size_t count) {
  std::mt19937 rng(4242u);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::vector<glm::vec3> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const float z = unit(rng) * 2.0f - 1.0f;
    const float theta = unit(rng) * 6.2831853f;
    const float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
    out.push_back(glm::vec3(r * std::cos(theta), r * std::sin(theta), z));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Half precision
// ---------------------------------------------------------------------------

TEST(GlmQuantize, HalfVec3IsSixBytes) {
  auto buffer = MakeBuffer();
  ext::WriteVecHalf(*buffer, glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(buffer->size(), 6u);
}

TEST(GlmQuantize, HalfRoundTripsWithinPrecision) {
  auto buffer = MakeBuffer();
  const glm::vec3 value(1.5f, -128.25f, 0.0009765625f);
  ext::WriteVecHalf(*buffer, value);

  const auto read = ext::ReadVecHalf<glm::vec3>(*buffer);
  // binary16 has 11 bits of mantissa, so the error is relative
  EXPECT_NEAR(read.x, value.x, std::fabs(value.x) * 1e-3f);
  EXPECT_NEAR(read.y, value.y, std::fabs(value.y) * 1e-3f);
  EXPECT_NEAR(read.z, value.z, std::fabs(value.z) * 1e-3f);
}

TEST(GlmQuantize, HalfKeepsExactSmallIntegers) {
  auto buffer = MakeBuffer();
  ext::WriteVecHalf(*buffer, glm::vec3(0.0f, -1.0f, 2048.0f));
  const auto read = ext::ReadVecHalf<glm::vec3>(*buffer);
  EXPECT_FLOAT_EQ(read.x, 0.0f);
  EXPECT_FLOAT_EQ(read.y, -1.0f);
  EXPECT_FLOAT_EQ(read.z, 2048.0f);
}

// ---------------------------------------------------------------------------
// Fixed point over a range
// ---------------------------------------------------------------------------

TEST(GlmQuantize, RangedEndpointsAreExact) {
  EXPECT_EQ(ext::QuantizeFloat<uint16_t>(-1000.0f, -1000.0f, 1000.0f), 0u);
  EXPECT_EQ(ext::QuantizeFloat<uint16_t>(1000.0f, -1000.0f, 1000.0f), 65535u);
  EXPECT_FLOAT_EQ(ext::DequantizeFloat<uint16_t>(0u, -1000.0f, 1000.0f),
                  -1000.0f);
  EXPECT_FLOAT_EQ(ext::DequantizeFloat<uint16_t>(65535u, -1000.0f, 1000.0f),
                  1000.0f);
}

TEST(GlmQuantize, RangedClampsRatherThanWraps) {
  EXPECT_EQ(ext::QuantizeFloat<uint8_t>(-5000.0f, -1.0f, 1.0f), 0u);
  EXPECT_EQ(ext::QuantizeFloat<uint8_t>(5000.0f, -1.0f, 1.0f), 255u);
}

TEST(GlmQuantize, RangedDegenerateRangeIsZero) {
  EXPECT_EQ(ext::QuantizeFloat<uint16_t>(5.0f, 3.0f, 3.0f), 0u);
  EXPECT_EQ(ext::QuantizeFloat<uint16_t>(5.0f, 10.0f, 0.0f), 0u);
}

// the documented bound: (max - min) / (2 * UInt_max).
TEST(GlmQuantize, RangedErrorStaysWithinHalfAStep) {
  const float min = -1000.0f;
  const float max = 1000.0f;
  const float tolerance = (max - min) / (2.0f * 65535.0f);

  std::mt19937 rng(7u);
  std::uniform_real_distribution<float> dist(min, max);
  for (int i = 0; i < 4096; ++i) {
    const float value = dist(rng);
    const float read = ext::DequantizeFloat<uint16_t>(
        ext::QuantizeFloat<uint16_t>(value, min, max), min, max);
    ASSERT_NEAR(read, value, tolerance) << "value " << value;
  }
}

TEST(GlmQuantize, RangedVec3RoundTripsThroughBuffer) {
  auto buffer = MakeBuffer();
  const glm::vec3 value(123.5f, -800.25f, 0.0f);
  ext::WriteVecRanged<uint16_t>(*buffer, value, -1000.0f, 1000.0f);

  EXPECT_EQ(buffer->size(), 6u);
  const auto read =
      ext::ReadVecRanged<uint16_t, glm::vec3>(*buffer, -1000.0f, 1000.0f);
  const float tolerance = 2000.0f / (2.0f * 65535.0f);
  EXPECT_NEAR(read.x, value.x, tolerance);
  EXPECT_NEAR(read.y, value.y, tolerance);
  EXPECT_NEAR(read.z, value.z, tolerance);
}

TEST(GlmQuantize, RangedWidthsCostWhatTheyClaim) {
  auto buffer = MakeBuffer();
  ext::WriteVecRanged<uint8_t>(*buffer, glm::vec3(0.0f), -1.0f, 1.0f);
  EXPECT_EQ(buffer->size(), 3u);

  auto wide = MakeBuffer();
  ext::WriteVecRanged<uint32_t>(*wide, glm::vec3(0.0f), -1.0f, 1.0f);
  EXPECT_EQ(wide->size(), 12u);
}

// ---------------------------------------------------------------------------
// Smallest-three quaternions
// ---------------------------------------------------------------------------

TEST(GlmQuantize, SmallestThreeIsFourBytes) {
  auto buffer = MakeBuffer();
  ext::WriteQuatSmallestThree(*buffer, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
  EXPECT_EQ(buffer->size(), 4u);
}

// identity survives bit-exactly, which is why the encoder uses an odd number
// of quantisation levels. With an even count zero falls between two codes and
// a resting object jitters by 0.137 degrees for free.
TEST(GlmQuantize, SmallestThreeKeepsIdentityExactly) {
  const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
  const glm::quat read =
      ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(identity));
  EXPECT_FLOAT_EQ(read.w, 1.0f);
  EXPECT_FLOAT_EQ(read.x, 0.0f);
  EXPECT_FLOAT_EQ(read.y, 0.0f);
  EXPECT_FLOAT_EQ(read.z, 0.0f);
  EXPECT_EQ(RotationAngleDegrees(read, identity), 0.0f);
}

TEST(GlmQuantize, SmallestThreeKeepsAxisAlignedRotations) {
  const glm::vec3 axes[3] = {glm::vec3(1.0f, 0.0f, 0.0f),
                             glm::vec3(0.0f, 1.0f, 0.0f),
                             glm::vec3(0.0f, 0.0f, 1.0f)};
  for (int a = 0; a < 3; ++a) {
    for (int step = 0; step < 8; ++step) {
      const float angle = static_cast<float>(step) * 0.7853981634f;
      const glm::quat q = glm::angleAxis(angle, axes[a]);
      const glm::quat read =
          ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(q));
      ASSERT_LT(RotationAngleDegrees(read, q), 0.15f)
          << "axis " << a << " step " << step;
    }
  }
}

TEST(GlmQuantize, SmallestThreeAccuracyOverTheWholeSphere) {
  float worst = 0.0f;
  for (const glm::quat& q : SampleQuats(20000)) {
    const glm::quat read =
        ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(q));
    const float error = RotationAngleDegrees(read, q);
    ASSERT_TRUE(std::isfinite(error));
    worst = std::fmax(worst, error);
  }
  // measured worst case over 300k uniform rotations is 0.115 degrees
  EXPECT_LT(worst, 0.15f) << "worst-case error " << worst << " degrees";
}

// q and -q are the same rotation. The encoder normalises the sign so both
// produce a decodable result naming that rotation.
TEST(GlmQuantize, SmallestThreeIsSignAgnostic) {
  for (const glm::quat& q : SampleQuats(256)) {
    const glm::quat negated(-q.w, -q.x, -q.y, -q.z);
    const glm::quat a =
        ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(q));
    const glm::quat b =
        ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(negated));
    ASSERT_LT(RotationAngleDegrees(a, b), 1e-6f);
  }
}

TEST(GlmQuantize, SmallestThreeNormalisesItsInput) {
  const glm::quat scaled(4.0f, 0.0f, 0.0f, 0.0f);  // 4 * identity
  const glm::quat read =
      ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(scaled));
  EXPECT_NEAR(glm::length(read), 1.0f, 1e-6f);
  EXPECT_EQ(RotationAngleDegrees(read, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), 0.0f);
}

TEST(GlmQuantize, SmallestThreeDegenerateInputIsIdentity) {
  const glm::quat zero(0.0f, 0.0f, 0.0f, 0.0f);
  const glm::quat read =
      ext::UnpackQuatSmallestThree(ext::PackQuatSmallestThree(zero));
  EXPECT_EQ(RotationAngleDegrees(read, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), 0.0f);
}

// any 32-bit word must decode to a finite unit quaternion; the reader does not
// get to trust that the sender was well behaved.
TEST(GlmQuantize, SmallestThreeSurvivesArbitraryWords) {
  std::mt19937 rng(99u);
  for (int i = 0; i < 10000; ++i) {
    const glm::quat read =
        ext::UnpackQuatSmallestThree(static_cast<uint32_t>(rng()));
    ASSERT_TRUE(std::isfinite(read.x) && std::isfinite(read.y) &&
                std::isfinite(read.z) && std::isfinite(read.w));
    ASSERT_NEAR(glm::length(read), 1.0f, 1e-6f);
  }
}

TEST(GlmQuantize, SmallestThreeRoundTripsThroughBuffer) {
  auto buffer = MakeBuffer();
  const glm::quat q = glm::normalize(glm::quat(0.3f, 0.5f, -0.4f, 0.7f));
  ext::WriteQuatSmallestThree(*buffer, q);
  EXPECT_LT(RotationAngleDegrees(ext::ReadQuatSmallestThree(*buffer), q), 0.15f);
}

// ---------------------------------------------------------------------------
// Octahedral normals
// ---------------------------------------------------------------------------

TEST(GlmQuantize, OctSizes) {
  auto wide = MakeBuffer();
  ext::WriteNormalOct<uint16_t>(*wide, glm::vec3(0.0f, 0.0f, 1.0f));
  EXPECT_EQ(wide->size(), 4u);

  auto narrow = MakeBuffer();
  ext::WriteNormalOct<uint8_t>(*narrow, glm::vec3(0.0f, 0.0f, 1.0f));
  EXPECT_EQ(narrow->size(), 2u);
}

TEST(GlmQuantize, Oct16BitAccuracyOverTheSphere) {
  float worst = 0.0f;
  for (const glm::vec3& n : SampleUnitVectors(20000)) {
    auto buffer = MakeBuffer();
    ext::WriteNormalOct<uint16_t>(*buffer, n);
    const glm::vec3 read = ext::ReadNormalOct<uint16_t>(*buffer);
    worst = std::fmax(worst, AngleBetweenDegrees(read, n));
  }
  // measured worst case over 300k uniform directions is 0.0037 degrees
  EXPECT_LT(worst, 0.01f) << "worst-case error " << worst << " degrees";
}

TEST(GlmQuantize, Oct8BitAccuracyOverTheSphere) {
  float worst = 0.0f;
  for (const glm::vec3& n : SampleUnitVectors(20000)) {
    auto buffer = MakeBuffer();
    ext::WriteNormalOct<uint8_t>(*buffer, n);
    const glm::vec3 read = ext::ReadNormalOct<uint8_t>(*buffer);
    worst = std::fmax(worst, AngleBetweenDegrees(read, n));
  }
  // measured worst case over 300k uniform directions is 0.94 degrees
  EXPECT_LT(worst, 1.2f) << "worst-case error " << worst << " degrees";
}

// the poles and the fold seam are where a naive x/y-plus-sign scheme breaks
// down, so they get their own check.
TEST(GlmQuantize, OctHandlesAxesAndSeams) {
  const glm::vec3 cases[] = {
      glm::vec3(1.0f, 0.0f, 0.0f),   glm::vec3(-1.0f, 0.0f, 0.0f),
      glm::vec3(0.0f, 1.0f, 0.0f),   glm::vec3(0.0f, -1.0f, 0.0f),
      glm::vec3(0.0f, 0.0f, 1.0f),   glm::vec3(0.0f, 0.0f, -1.0f),
      glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)),
      glm::normalize(glm::vec3(-1.0f, 1.0f, 0.0f)),
      glm::normalize(glm::vec3(1.0f, -1.0f, -0.0001f)),
      glm::normalize(glm::vec3(-1.0f, -1.0f, -0.0001f)),
  };
  for (const glm::vec3& n : cases) {
    auto buffer = MakeBuffer();
    ext::WriteNormalOct<uint16_t>(*buffer, n);
    const glm::vec3 read = ext::ReadNormalOct<uint16_t>(*buffer);
    ASSERT_LT(AngleBetweenDegrees(read, n), 0.01f)
        << "normal " << n.x << ", " << n.y << ", " << n.z;
  }
}

TEST(GlmQuantize, OctNormalisesItsInput) {
  auto buffer = MakeBuffer();
  ext::WriteNormalOct<uint16_t>(*buffer, glm::vec3(0.0f, 0.0f, 7.0f));
  const glm::vec3 read = ext::ReadNormalOct<uint16_t>(*buffer);
  EXPECT_NEAR(glm::length(read), 1.0f, 1e-4f);
  EXPECT_LT(AngleBetweenDegrees(read, glm::vec3(0.0f, 0.0f, 1.0f)), 0.01f);
}

TEST(GlmQuantize, OctZeroVectorDecodesToAFiniteUnitVector) {
  auto buffer = MakeBuffer();
  ext::WriteNormalOct<uint16_t>(*buffer, glm::vec3(0.0f, 0.0f, 0.0f));
  const glm::vec3 read = ext::ReadNormalOct<uint16_t>(*buffer);
  EXPECT_TRUE(std::isfinite(read.x) && std::isfinite(read.y) &&
              std::isfinite(read.z));
  EXPECT_NEAR(glm::length(read), 1.0f, 1e-4f);
}

TEST(GlmQuantize, OctSurvivesArbitraryBytes) {
  std::mt19937 rng(11u);
  for (int i = 0; i < 10000; ++i) {
    auto buffer = MakeBuffer();
    buffer->WriteInt<uint16_t>(static_cast<uint16_t>(rng()));
    buffer->WriteInt<uint16_t>(static_cast<uint16_t>(rng()));
    const glm::vec3 read = ext::ReadNormalOct<uint16_t>(*buffer);
    ASSERT_NEAR(glm::length(read), 1.0f, 1e-6f);
  }
}

// ---------------------------------------------------------------------------
// What the whole thing is for
// ---------------------------------------------------------------------------

TEST(GlmQuantize, TransformUpdateShrinksFrom40BytesTo14) {
  const glm::vec3 position(120.5f, -3.25f, 88.0f);
  const glm::quat rotation = glm::normalize(glm::quat(0.3f, 0.5f, -0.4f, 0.7f));
  const glm::vec3 normal = glm::normalize(glm::vec3(0.2f, -0.9f, 0.4f));

  auto exact = MakeBuffer();
  ext::WriteVec(*exact, position);
  ext::WriteQuat(*exact, rotation);
  ext::WriteVec(*exact, normal);
  EXPECT_EQ(exact->size(), 40u);

  auto packed = MakeBuffer();
  ext::WriteVecRanged<uint16_t>(*packed, position, -1024.0f, 1024.0f);
  ext::WriteQuatSmallestThree(*packed, rotation);
  ext::WriteNormalOct<uint16_t>(*packed, normal);
  EXPECT_EQ(packed->size(), 14u);

  const auto read_position =
      ext::ReadVecRanged<uint16_t, glm::vec3>(*packed, -1024.0f, 1024.0f);
  const auto read_rotation = ext::ReadQuatSmallestThree(*packed);
  const auto read_normal = ext::ReadNormalOct<uint16_t>(*packed);

  EXPECT_NEAR(read_position.x, position.x, 0.04f);
  EXPECT_NEAR(read_position.y, position.y, 0.04f);
  EXPECT_NEAR(read_position.z, position.z, 0.04f);
  EXPECT_LT(RotationAngleDegrees(read_rotation, rotation), 0.15f);
  EXPECT_LT(AngleBetweenDegrees(read_normal, normal), 0.01f);
}
