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
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "znet/ext/quantize/quantize.h"

using znet::ext::quant::DequantizeFromInt;
using znet::ext::quant::DequantizeFromLevels;
using znet::ext::quant::LevelsForBits;
using znet::ext::quant::PackAngle;
using znet::ext::quant::PackDirectionOct;
using znet::ext::quant::PackHalf;
using znet::ext::quant::PackQuatSmallestThree;
using znet::ext::quant::Quat4;
using znet::ext::quant::QuantizeToInt;
using znet::ext::quant::QuantizeToLevels;
using znet::ext::quant::UnpackAngle;
using znet::ext::quant::UnpackDirectionOct;
using znet::ext::quant::UnpackHalf;
using znet::ext::quant::UnpackQuatSmallestThree;
using znet::ext::quant::Vec3f;

namespace {

const float kRadiansToDegrees = 57.29577951308232f;

/** @brief Stable angle between rotations; acos(dot) is useless this near zero. */
float RotationAngleDegrees(const Quat4& a, const Quat4& b) {
  const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  const float s = dot < 0.0f ? -1.0f : 1.0f;
  const float dx = a.x - b.x * s;
  const float dy = a.y - b.y * s;
  const float dz = a.z - b.z * s;
  const float dw = a.w - b.w * s;
  const float sx = a.x + b.x * s;
  const float sy = a.y + b.y * s;
  const float sz = a.z + b.z * s;
  const float sw = a.w + b.w * s;
  return 2.0f *
         std::atan2(std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw),
                    std::sqrt(sx * sx + sy * sy + sz * sz + sw * sw)) *
         kRadiansToDegrees;
}

float DirectionAngleDegrees(const Vec3f& a, const Vec3f& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  const float sx = a.x + b.x;
  const float sy = a.y + b.y;
  const float sz = a.z + b.z;
  return 2.0f *
         std::atan2(std::sqrt(dx * dx + dy * dy + dz * dz),
                    std::sqrt(sx * sx + sy * sy + sz * sz)) *
         kRadiansToDegrees;
}

float Length(const Quat4& q) {
  return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

float Length(const Vec3f& v) {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

std::vector<Quat4> SampleQuats(size_t count) {
  std::mt19937 rng(1337u);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::vector<Quat4> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const float u1 = unit(rng);
    const float u2 = unit(rng) * 6.2831853f;
    const float u3 = unit(rng) * 6.2831853f;
    const float s1 = std::sqrt(1.0f - u1);
    const float s2 = std::sqrt(u1);
    Quat4 q;
    q.x = s1 * std::sin(u2);
    q.y = s1 * std::cos(u2);
    q.z = s2 * std::sin(u3);
    q.w = s2 * std::cos(u3);
    out.push_back(q);
  }
  return out;
}

std::vector<Vec3f> SampleDirections(size_t count) {
  std::mt19937 rng(4242u);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::vector<Vec3f> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const float z = unit(rng) * 2.0f - 1.0f;
    const float theta = unit(rng) * 6.2831853f;
    const float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
    Vec3f v;
    v.x = r * std::cos(theta);
    v.y = r * std::sin(theta);
    v.z = z;
    out.push_back(v);
  }
  return out;
}

/**
 * @brief An independent binary16 encoder, written for clarity not speed.
 *
 * PackHalf is bit-twiddling with three rounding paths, which is exactly the
 * kind of code that looks right and is not. This reference derives the same
 * answer a different way, through frexp and ldexp, so agreeing with it means
 * something.
 */
uint16_t ReferencePackHalf(float value) {
  if (std::isnan(value)) {
    return 0x7E00u;
  }
  const uint32_t sign = std::signbit(value) ? 0x8000u : 0x0000u;
  const float magnitude = std::fabs(value);
  if (std::isinf(magnitude)) {
    return static_cast<uint16_t>(sign | 0x7C00u);
  }
  if (magnitude == 0.0f) {
    return static_cast<uint16_t>(sign);
  }
  // smallest positive binary16 subnormal is 2^-24
  const double scaled = static_cast<double>(magnitude) / std::ldexp(1.0, -24);
  if (magnitude < std::ldexp(1.0f, -14)) {
    double rounded = std::nearbyint(scaled);
    if (rounded > 1023.0) {  // rounded up into the smallest normal
      return static_cast<uint16_t>(sign | 0x0400u);
    }
    return static_cast<uint16_t>(sign | static_cast<uint32_t>(rounded));
  }
  int exponent = 0;
  const double fraction = std::frexp(static_cast<double>(magnitude), &exponent);
  // frexp gives [0.5, 1); binary16 wants [1, 2) with a 10-bit mantissa
  double mantissa = std::nearbyint((fraction * 2.0 - 1.0) * 1024.0);
  int biased = exponent - 1 + 15;
  if (mantissa == 1024.0) {
    mantissa = 0.0;
    ++biased;
  }
  if (biased >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00u);
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(biased) << 10) |
                               static_cast<uint32_t>(mantissa));
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixed point
// ---------------------------------------------------------------------------

TEST(Quantize, EndpointsAreExact) {
  EXPECT_EQ(QuantizeToLevels(-1000.0f, -1000.0f, 1000.0f, 65535), 0u);
  EXPECT_EQ(QuantizeToLevels(1000.0f, -1000.0f, 1000.0f, 65535), 65535u);
  EXPECT_FLOAT_EQ(DequantizeFromLevels(0, -1000.0f, 1000.0f, 65535), -1000.0f);
  EXPECT_FLOAT_EQ(DequantizeFromLevels(65535, -1000.0f, 1000.0f, 65535),
                  1000.0f);
}

TEST(Quantize, ClampsRatherThanWraps) {
  EXPECT_EQ(QuantizeToLevels(-5000.0f, -1.0f, 1.0f, 255), 0u);
  EXPECT_EQ(QuantizeToLevels(5000.0f, -1.0f, 1.0f, 255), 255u);
}

TEST(Quantize, DegenerateInputsYieldZero) {
  EXPECT_EQ(QuantizeToLevels(5.0f, 3.0f, 3.0f, 255), 0u);
  EXPECT_EQ(QuantizeToLevels(5.0f, 10.0f, 0.0f, 255), 0u);
  EXPECT_EQ(QuantizeToLevels(5.0f, 0.0f, 10.0f, 0), 0u);
  EXPECT_FLOAT_EQ(DequantizeFromLevels(7, 0.0f, 10.0f, 0), 0.0f);
}

TEST(Quantize, LevelsForBitsIsExact) {
  EXPECT_EQ(LevelsForBits(0), 0u);
  EXPECT_EQ(LevelsForBits(1), 1u);
  EXPECT_EQ(LevelsForBits(8), 255u);
  EXPECT_EQ(LevelsForBits(16), 65535u);
  EXPECT_EQ(LevelsForBits(32), 4294967295u);
  EXPECT_EQ(LevelsForBits(64), std::numeric_limits<uint64_t>::max());
}

TEST(Quantize, ErrorStaysWithinHalfAStepPlusFloatPrecision) {
  const float min = -1000.0f;
  const float max = 1000.0f;
  const uint64_t levels = 65535;
  const float tolerance = (max - min) / (2.0f * static_cast<float>(levels)) +
                          std::fabs(max) * std::numeric_limits<float>::epsilon();

  std::mt19937 rng(7u);
  std::uniform_real_distribution<float> dist(min, max);
  for (int i = 0; i < 20000; ++i) {
    const float value = dist(rng);
    const float read = DequantizeFromLevels(
        QuantizeToLevels(value, min, max, levels), min, max, levels);
    ASSERT_NEAR(read, value, tolerance) << "value " << value;
  }
}

// the typed helpers are the same arithmetic, so they must agree exactly with
// the level-based form: this is what lets glm and bitpack share one codec.
TEST(Quantize, TypedHelpersMatchTheLevelForm) {
  std::mt19937 rng(11u);
  std::uniform_real_distribution<float> dist(-10.0f, 10.0f);
  for (int i = 0; i < 5000; ++i) {
    const float value = dist(rng);
    EXPECT_EQ(static_cast<uint64_t>(QuantizeToInt<uint16_t>(value, -10.0f, 10.0f)),
              QuantizeToLevels(value, -10.0f, 10.0f, 65535));
    EXPECT_EQ(static_cast<uint64_t>(QuantizeToInt<uint8_t>(value, -10.0f, 10.0f)),
              QuantizeToLevels(value, -10.0f, 10.0f, 255));
  }
  EXPECT_FLOAT_EQ(DequantizeFromInt<uint16_t>(12345u, -10.0f, 10.0f),
                  DequantizeFromLevels(12345u, -10.0f, 10.0f, 65535));
}

// ---------------------------------------------------------------------------
// Half precision
// ---------------------------------------------------------------------------

// every one of the 65536 binary16 bit patterns must survive decode/encode, so
// the two directions cannot disagree anywhere in the format, including the
// subnormals and both zeroes.
TEST(Half, EveryBitPatternRoundTrips) {
  for (uint32_t raw = 0; raw < 0x10000u; ++raw) {
    const uint16_t bits = static_cast<uint16_t>(raw);
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;
    if (exponent == 0x1Fu && mantissa != 0) {
      continue;  // naN payloads are not required to be preserved
    }
    const float decoded = UnpackHalf(bits);
    ASSERT_EQ(PackHalf(decoded), bits) << "pattern 0x" << std::hex << raw;
  }
}

TEST(Half, MatchesAnIndependentReference) {
  std::mt19937 rng(2026u);
  int checked = 0;
  for (int i = 0; i < 400000; ++i) {
    const uint32_t raw = static_cast<uint32_t>(rng() & 0xFFFFFFFFu);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    if (std::isnan(value) || std::isinf(value)) {
      continue;
    }
    ASSERT_EQ(PackHalf(value), ReferencePackHalf(value))
        << "value " << value << " raw 0x" << std::hex << raw;
    ++checked;
  }
  EXPECT_GT(checked, 100000);
}

TEST(Half, EdgeMagnitudesMatchTheReference) {
  const float values[] = {0.0f,
                          -0.0f,
                          1.0f,
                          -1.0f,
                          65504.0f,     // largest finite binary16
                          -65504.0f,
                          65520.0f,     // rounds up to infinity
                          131072.0f,    // overflows
                          -131072.0f,
                          6.103515625e-05f,   // smallest normal
                          6.0e-05f,           // subnormal
                          5.960464477539063e-08f,  // smallest subnormal
                          2.9802322387695312e-08f, // rounds to zero or min
                          1.0e-10f,
                          2048.0f,
                          2049.0f};
  for (const float value : values) {
    EXPECT_EQ(PackHalf(value), ReferencePackHalf(value)) << "value " << value;
  }
}

TEST(Half, SaturatesRatherThanWrappingOnOverflow) {
  EXPECT_EQ(PackHalf(1.0e30f), 0x7C00u);
  EXPECT_EQ(PackHalf(-1.0e30f), 0xFC00u);
  EXPECT_TRUE(std::isinf(UnpackHalf(0x7C00u)));
  EXPECT_GT(UnpackHalf(0x7C00u), 0.0f);
}

TEST(Half, NaNStaysNaN) {
  EXPECT_TRUE(std::isnan(UnpackHalf(PackHalf(
      std::numeric_limits<float>::quiet_NaN()))));
}

// ---------------------------------------------------------------------------
// Rotations
// ---------------------------------------------------------------------------

TEST(SmallestThree, IdentityIsExact) {
  const Quat4 identity;
  const Quat4 read = UnpackQuatSmallestThree(PackQuatSmallestThree(identity));
  EXPECT_FLOAT_EQ(read.w, 1.0f);
  EXPECT_FLOAT_EQ(read.x, 0.0f);
  EXPECT_FLOAT_EQ(read.y, 0.0f);
  EXPECT_FLOAT_EQ(read.z, 0.0f);
}

TEST(SmallestThree, AccuracyOverTheWholeSphere) {
  float worst = 0.0f;
  for (const Quat4& q : SampleQuats(50000)) {
    const float error =
        RotationAngleDegrees(UnpackQuatSmallestThree(PackQuatSmallestThree(q)), q);
    ASSERT_TRUE(std::isfinite(error));
    worst = std::fmax(worst, error);
  }
  EXPECT_LT(worst, 0.15f) << "worst-case error " << worst << " degrees";
}

TEST(SmallestThree, IsSignAgnostic) {
  for (const Quat4& q : SampleQuats(512)) {
    Quat4 negated;
    negated.x = -q.x;
    negated.y = -q.y;
    negated.z = -q.z;
    negated.w = -q.w;
    EXPECT_EQ(PackQuatSmallestThree(q), PackQuatSmallestThree(negated));
  }
}

TEST(SmallestThree, NormalisesAndSurvivesDegenerateInput) {
  Quat4 scaled;
  scaled.w = 4.0f;
  EXPECT_NEAR(Length(UnpackQuatSmallestThree(PackQuatSmallestThree(scaled))),
              1.0f, 1e-6f);

  Quat4 zero;
  zero.w = 0.0f;
  const Quat4 read = UnpackQuatSmallestThree(PackQuatSmallestThree(zero));
  EXPECT_NEAR(Length(read), 1.0f, 1e-6f);
}

TEST(SmallestThree, AnyWordDecodesToAUnitQuaternion) {
  std::mt19937 rng(99u);
  for (int i = 0; i < 50000; ++i) {
    const Quat4 read =
        UnpackQuatSmallestThree(static_cast<uint32_t>(rng() & 0xFFFFFFFFu));
    ASSERT_TRUE(std::isfinite(read.x) && std::isfinite(read.y) &&
                std::isfinite(read.z) && std::isfinite(read.w));
    ASSERT_NEAR(Length(read), 1.0f, 1e-3f);
  }
}

// ---------------------------------------------------------------------------
// Planar angles
// ---------------------------------------------------------------------------

TEST(Angle, RoundTripsWithinHalfAStep) {
  for (unsigned bits : {8u, 10u, 12u, 16u}) {
    const double levels = static_cast<double>(LevelsForBits(bits) + 1);
    const float tolerance = static_cast<float>(6.283185307179586 / levels);
    std::mt19937 rng(bits);
    std::uniform_real_distribution<float> dist(-3.14159265f, 3.14159265f);
    for (int i = 0; i < 2000; ++i) {
      const float angle = dist(rng);
      const float read = UnpackAngle(PackAngle(angle, bits), bits);
      // compare as a circular difference so the seam does not read as an error
      float difference = read - angle;
      while (difference > 3.14159265f) difference -= 6.28318531f;
      while (difference < -3.14159265f) difference += 6.28318531f;
      ASSERT_LT(std::fabs(difference), tolerance)
          << "bits " << bits << " angle " << angle;
    }
  }
}

// an angle is a circle, not an interval. A body spinning through the wrap must
// not jump a whole quantisation step at the seam, which is what happens if
// +pi and -pi get different codes.
TEST(Angle, HasNoSeamAtTheWrap) {
  const unsigned bits = 12;
  EXPECT_EQ(PackAngle(3.14159265f, bits), PackAngle(-3.14159265f, bits));
  EXPECT_EQ(PackAngle(0.0f, bits), PackAngle(6.28318531f, bits));
  EXPECT_EQ(PackAngle(0.1f, bits), PackAngle(0.1f + 6.28318531f, bits));
  EXPECT_EQ(PackAngle(0.1f, bits), PackAngle(0.1f - 6.28318531f, bits));
}

TEST(Angle, StaysInRangeForAnyCode) {
  const unsigned bits = 10;
  for (uint32_t code = 0; code < 4096; ++code) {
    const float angle = UnpackAngle(code, bits);
    ASSERT_GE(angle, -3.1415927f);
    ASSERT_LT(angle, 3.1415927f);
  }
}

TEST(Angle, ZeroWidthIsHarmless) {
  EXPECT_EQ(PackAngle(1.0f, 0), 0u);
  EXPECT_FLOAT_EQ(UnpackAngle(0, 0), 0.0f);
}

// ---------------------------------------------------------------------------
// Directions
// ---------------------------------------------------------------------------

TEST(Direction, SixteenBitAccuracy) {
  float worst = 0.0f;
  for (const Vec3f& n : SampleDirections(50000)) {
    worst = std::fmax(
        worst, DirectionAngleDegrees(
                   UnpackDirectionOct(PackDirectionOct(n, 16), 16), n));
  }
  EXPECT_LT(worst, 0.01f) << "worst-case error " << worst << " degrees";
}

TEST(Direction, EightBitAccuracy) {
  float worst = 0.0f;
  for (const Vec3f& n : SampleDirections(50000)) {
    worst = std::fmax(
        worst,
        DirectionAngleDegrees(UnpackDirectionOct(PackDirectionOct(n, 8), 8), n));
  }
  EXPECT_LT(worst, 1.2f) << "worst-case error " << worst << " degrees";
}

TEST(Direction, HandlesAxesAndFoldSeams) {
  const Vec3f cases[] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f},
                         {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
                         {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
                         {0.707f, 0.707f, 0.0f}, {-0.707f, 0.707f, 0.0f},
                         {0.707f, -0.707f, -0.0001f},
                         {-0.707f, -0.707f, -0.0001f}};
  for (const Vec3f& n : cases) {
    const Vec3f read = UnpackDirectionOct(PackDirectionOct(n, 16), 16);
    ASSERT_LT(DirectionAngleDegrees(read, n), 0.02f)
        << "direction " << n.x << ", " << n.y << ", " << n.z;
  }
}

TEST(Direction, ZeroVectorIsFinite) {
  const Vec3f zero;
  const Vec3f read = UnpackDirectionOct(PackDirectionOct(zero, 16), 16);
  EXPECT_TRUE(std::isfinite(read.x) && std::isfinite(read.y) &&
              std::isfinite(read.z));
  EXPECT_NEAR(Length(read), 1.0f, 1e-4f);
}

TEST(Direction, AnyWordDecodesToAUnitVector) {
  std::mt19937 rng(31u);
  for (int i = 0; i < 50000; ++i) {
    const Vec3f read =
        UnpackDirectionOct(static_cast<uint32_t>(rng() & 0xFFFFFFFFu), 16);
    ASSERT_NEAR(Length(read), 1.0f, 1e-3f);
  }
}

TEST(Direction, PackedWidthIsAsClaimed) {
  const Vec3f n{0.0f, 0.0f, 1.0f};
  EXPECT_LE(PackDirectionOct(n, 8), 0xFFFFu);
  EXPECT_LE(static_cast<uint64_t>(PackDirectionOct(n, 16)), 0xFFFFFFFFull);
}
