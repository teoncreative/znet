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
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "znet/ext/bitpack/bitpack.h"

using znet::Buffer;
using znet::BufferError;
using znet::Endianness;
using znet::ext::BitReader;
using znet::ext::BitWriter;
using znet::ext::BitsForRange;
using znet::ext::BitsForSignedRange;

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

}  // namespace

// ---------------------------------------------------------------------------
// Field widths
// ---------------------------------------------------------------------------

TEST(BitsForRange, KnownWidths) {
  EXPECT_EQ(BitsForRange(0, 0), 0u);       // one value carries nothing
  EXPECT_EQ(BitsForRange(7, 7), 0u);
  EXPECT_EQ(BitsForRange(0, 1), 1u);
  EXPECT_EQ(BitsForRange(0, 2), 2u);
  EXPECT_EQ(BitsForRange(0, 3), 2u);
  EXPECT_EQ(BitsForRange(0, 4), 3u);
  EXPECT_EQ(BitsForRange(0, 255), 8u);
  EXPECT_EQ(BitsForRange(0, 256), 9u);
  EXPECT_EQ(BitsForRange(0, 1000), 10u);
  EXPECT_EQ(BitsForRange(1000, 2000), 10u);  // width follows the span
  EXPECT_EQ(BitsForRange(0, UINT64_MAX), 64u);
}

TEST(BitsForRange, ResolvesAtCompileTime) {
  static_assert(BitsForRange(0, 1000) == 10u, "should fold");
  static_assert(BitsForSignedRange(-50, 100) == 8u, "should fold");
  SUCCEED();
}

TEST(BitsForRange, SignedRangesDoNotOverflow) {
  EXPECT_EQ(BitsForSignedRange(-1, 1), 2u);
  EXPECT_EQ(BitsForSignedRange(-50, 100), 8u);
  EXPECT_EQ(BitsForSignedRange(-128, 127), 8u);
  EXPECT_EQ(BitsForSignedRange(0, 0), 0u);
  // the widest possible signed range still answers rather than wrapping
  EXPECT_EQ(BitsForSignedRange(INT64_MIN, INT64_MAX), 64u);
}

// ---------------------------------------------------------------------------
// Bit order
// ---------------------------------------------------------------------------

// the documented layout: least significant bit first within a byte. Two fields
// of 3 and 2 bits land in one byte as 0b00011101.
TEST(BitPack, BitOrderIsLsbFirst) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0x5u, 3);  // 0b101
    bits.WriteBits(0x3u, 2);  // 0b11
  }
  ASSERT_EQ(buffer->size(), 1u);
  EXPECT_EQ(buffer->ReadInt<uint8_t>(), 0x1Du);  // 0b00011101
}

TEST(BitPack, PartialByteIsZeroPadded) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBool(true);
  }
  ASSERT_EQ(buffer->size(), 1u);
  EXPECT_EQ(buffer->ReadInt<uint8_t>(), 0x01u);
}

// ---------------------------------------------------------------------------
// What the extension is for
// ---------------------------------------------------------------------------

TEST(BitPack, PacksAPlayerStateIntoFourBytesInsteadOfThirteen) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBool(true);                       // firing:  1 bit  (was 8)
    bits.WriteUIntRanged(137, 0, 200);          // ammo:    8 bits (was 16)
    bits.WriteIntRanged(-12, -50, 100);         // health:  8 bits (was 32)
    bits.WriteFloatRanged(1.75f, -3.15f, 3.15f, 12);  // yaw: 12 bits (was 32)
    EXPECT_EQ(bits.bits_written(), 29u);
  }
  EXPECT_EQ(buffer->size(), 4u);

  BitReader bits(*buffer);
  EXPECT_TRUE(bits.ReadBool());
  EXPECT_EQ(bits.ReadUIntRanged(0, 200), 137u);
  EXPECT_EQ(bits.ReadIntRanged(-50, 100), -12);
  EXPECT_NEAR(bits.ReadFloatRanged(-3.15f, 3.15f, 12), 1.75f, 6.3f / 8190.0f);
  EXPECT_TRUE(bits.ok());
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(BitPack, EveryWidthRoundTrips) {
  for (unsigned width = 1; width <= 32; ++width) {
    const uint32_t all_ones =
        width >= 32 ? 0xFFFFFFFFu : ((UINT32_C(1) << width) - 1);
    const uint32_t values[] = {0u, 1u, all_ones, all_ones / 2u};
    for (uint32_t value : values) {
      auto buffer = MakeBuffer();
      {
        BitWriter bits(*buffer);
        bits.WriteBits(value, width);
      }
      BitReader bits(*buffer);
      ASSERT_EQ(bits.ReadBits(width), value) << "width " << width;
      ASSERT_TRUE(bits.ok());
    }
  }
}

TEST(BitPack, Bits64RoundTrips) {
  const uint64_t values[] = {0u,
                             1u,
                             0xFFFFFFFFull,
                             0x100000000ull,
                             0xDEADBEEFCAFEBABEull,
                             UINT64_MAX};
  for (unsigned width = 33; width <= 64; ++width) {
    for (uint64_t value : values) {
      const uint64_t masked =
          width >= 64 ? value : (value & ((UINT64_C(1) << width) - 1));
      auto buffer = MakeBuffer();
      {
        BitWriter bits(*buffer);
        bits.WriteBits64(masked, width);
      }
      BitReader bits(*buffer);
      ASSERT_EQ(bits.ReadBits64(width), masked) << "width " << width;
    }
  }
}

// a bit packer's bugs live at the boundaries between scratch flushes and
// between bytes, and they only show up for particular sequences of widths.
// random sequences are the only practical way to reach them.
TEST(BitPack, RandomFieldSequencesRoundTrip) {
  std::mt19937 rng(20260801u);
  std::uniform_int_distribution<unsigned> width_dist(1, 32);
  std::uniform_int_distribution<unsigned> count_dist(1, 200);

  for (int trial = 0; trial < 2000; ++trial) {
    const unsigned count = count_dist(rng);
    std::vector<unsigned> widths;
    std::vector<uint32_t> values;
    widths.reserve(count);
    values.reserve(count);

    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      for (unsigned i = 0; i < count; ++i) {
        const unsigned width = width_dist(rng);
        const uint32_t mask =
            width >= 32 ? 0xFFFFFFFFu : ((UINT32_C(1) << width) - 1);
        const uint32_t value = static_cast<uint32_t>(rng()) & mask;
        widths.push_back(width);
        values.push_back(value);
        bits.WriteBits(value, width);
      }
      ASSERT_TRUE(bits.ok());
    }

    BitReader bits(*buffer);
    for (unsigned i = 0; i < count; ++i) {
      ASSERT_EQ(bits.ReadBits(widths[i]), values[i])
          << "trial " << trial << " field " << i << " width " << widths[i];
    }
    ASSERT_TRUE(bits.ok());
  }
}

TEST(BitPack, ByteCountIsCeilingOfBitCount) {
  for (unsigned total = 1; total <= 200; ++total) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      for (unsigned i = 0; i < total; ++i) {
        bits.WriteBool((i % 3) == 0);
      }
      EXPECT_EQ(bits.bits_written(), total);
    }
    EXPECT_EQ(buffer->size(), (total + 7u) / 8u) << "after " << total << " bits";
  }
}

// ---------------------------------------------------------------------------
// Ranged fields
// ---------------------------------------------------------------------------

TEST(BitPack, UIntRangedRoundTripsEveryValue) {
  const uint64_t min = 1000;
  const uint64_t max = 1300;
  for (uint64_t value = min; value <= max; ++value) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteUIntRanged(value, min, max);
    }
    BitReader bits(*buffer);
    ASSERT_EQ(bits.ReadUIntRanged(min, max), value);
  }
}

TEST(BitPack, IntRangedRoundTripsEveryValue) {
  const int64_t min = -150;
  const int64_t max = 150;
  for (int64_t value = min; value <= max; ++value) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteIntRanged(value, min, max);
    }
    BitReader bits(*buffer);
    ASSERT_EQ(bits.ReadIntRanged(min, max), value);
  }
}

TEST(BitPack, RangedValuesClampRatherThanWrap) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteUIntRanged(5, 10, 20);      // below
    bits.WriteUIntRanged(9999, 10, 20);   // above
    bits.WriteIntRanged(-9999, -5, 5);
    bits.WriteIntRanged(9999, -5, 5);
  }
  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadUIntRanged(10, 20), 10u);
  EXPECT_EQ(bits.ReadUIntRanged(10, 20), 20u);
  EXPECT_EQ(bits.ReadIntRanged(-5, 5), -5);
  EXPECT_EQ(bits.ReadIntRanged(-5, 5), 5);
}

// a range with one possible value carries no information and should cost no
// bits, while still round-tripping.
TEST(BitPack, SingleValueRangeIsFree) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteUIntRanged(42, 42, 42);
    bits.WriteIntRanged(-7, -7, -7);
    EXPECT_EQ(bits.bits_written(), 0u);
  }
  EXPECT_EQ(buffer->size(), 0u);

  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadUIntRanged(42, 42), 42u);
  EXPECT_EQ(bits.ReadIntRanged(-7, -7), -7);
  EXPECT_TRUE(bits.ok());
}

TEST(BitPack, ExtremeRangesRoundTrip) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteUIntRanged(UINT64_MAX / 2, 0, UINT64_MAX);
    bits.WriteIntRanged(INT64_MIN, INT64_MIN, INT64_MAX);
    bits.WriteIntRanged(INT64_MAX, INT64_MIN, INT64_MAX);
    bits.WriteIntRanged(0, INT64_MIN, INT64_MAX);
  }
  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadUIntRanged(0, UINT64_MAX), UINT64_MAX / 2);
  EXPECT_EQ(bits.ReadIntRanged(INT64_MIN, INT64_MAX), INT64_MIN);
  EXPECT_EQ(bits.ReadIntRanged(INT64_MIN, INT64_MAX), INT64_MAX);
  EXPECT_EQ(bits.ReadIntRanged(INT64_MIN, INT64_MAX), 0);
}

// the clamp on read is a safety property, not a nicety: a decoded value gets
// used as an array index. Every possible code must land inside the bounds.
TEST(BitPack, MalformedCodesStayInsideTheRange) {
  // [0, 5] needs 3 bits, so codes 6 and 7 are expressible but out of range
  for (uint32_t code = 0; code < 8; ++code) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteBits(code, 3);
    }
    BitReader bits(*buffer);
    const uint64_t value = bits.ReadUIntRanged(0, 5);
    ASSERT_LE(value, 5u) << "code " << code;
  }
  for (uint32_t code = 0; code < 8; ++code) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteBits(code, 3);
    }
    BitReader bits(*buffer);
    const int64_t value = bits.ReadIntRanged(-2, 3);
    ASSERT_GE(value, -2) << "code " << code;
    ASSERT_LE(value, 3) << "code " << code;
  }
}

// ---------------------------------------------------------------------------
// Ranged floats
// ---------------------------------------------------------------------------

TEST(BitPack, FloatRangedStaysWithinHalfAStep) {
  const float min = -3.15f;
  const float max = 3.15f;
  for (unsigned width = 4; width <= 24; width += 4) {
    // the codec's own level count, not a second derivation of it. a tolerance
    // built from a different number than the quantiser rounded against is
    // measuring something else, and the two only look alike until a compiler
    // folds one of them differently.
    const uint64_t levels = znet::ext::quant::LevelsForBits(width);

    // half a quantisation step, plus the float32 representation error of the
    // value being returned. In double, because at 24 bits over this range the
    // step is already near what a float can hold and a tolerance that rounds
    // is a tolerance that lies. Past about 21 bits the second term dominates:
    // the quantiser is finer than a float can hold, and spending more bits
    // buys nothing. See FloatRangedHitsFloatPrecisionAroundTwentyBits.
    const double half_step = (static_cast<double>(max) - static_cast<double>(min)) /
                             (2.0 * static_cast<double>(levels));
    const double tolerance =
        half_step +
        static_cast<double>(std::fabs(max)) *
            static_cast<double>(std::numeric_limits<float>::epsilon());
    std::mt19937 rng(width);
    std::uniform_real_distribution<float> dist(min, max);
    for (int i = 0; i < 500; ++i) {
      const float value = dist(rng);
      auto buffer = MakeBuffer();
      {
        BitWriter bits(*buffer);
        bits.WriteFloatRanged(value, min, max, width);
      }
      BitReader bits(*buffer);
      ASSERT_NEAR(bits.ReadFloatRanged(min, max, width), value, tolerance)
          << "width " << width << " levels " << levels << " half step "
          << half_step << " value " << value;
    }
  }
}

// where the useful width tops out. Over a range of ~6, a 24-bit field's step
// is finer than the gap between adjacent float32 values, so those extra bits
// buy no accuracy over a 21-bit field -- they just cost bandwidth.
TEST(BitPack, FloatRangedHitsFloatPrecisionAroundTwentyBits) {
  const float min = -3.15f;
  const float max = 3.15f;
  const float ulp_near_max = std::nextafter(max, 10.0f) - max;

  const float step_at_21 =
      (max - min) / static_cast<float>((UINT64_C(1) << 21) - 1);
  const float step_at_28 =
      (max - min) / static_cast<float>((UINT64_C(1) << 28) - 1);

  EXPECT_GT(step_at_21, ulp_near_max) << "21 bits should still be the limit";
  EXPECT_LT(step_at_28, ulp_near_max) << "28 bits is finer than a float";
}

TEST(BitPack, FloatRangedEndpointsAreExact) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteFloatRanged(-10.0f, -10.0f, 10.0f, 16);
    bits.WriteFloatRanged(10.0f, -10.0f, 10.0f, 16);
  }
  BitReader bits(*buffer);
  EXPECT_FLOAT_EQ(bits.ReadFloatRanged(-10.0f, 10.0f, 16), -10.0f);
  EXPECT_FLOAT_EQ(bits.ReadFloatRanged(-10.0f, 10.0f, 16), 10.0f);
}

// a degenerate range must still spend its bits, or the reader's field
// alignment would depend on values the sender happened to be holding.
TEST(BitPack, DegenerateFloatRangeStillCostsItsBits) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteFloatRanged(5.0f, 3.0f, 3.0f, 12);
    bits.WriteBits(0xABCu, 12);
    EXPECT_EQ(bits.bits_written(), 24u);
  }
  BitReader bits(*buffer);
  bits.ReadFloatRanged(3.0f, 3.0f, 12);
  EXPECT_EQ(bits.ReadBits(12), 0xABCu);  // stayed in sync
}

// ---------------------------------------------------------------------------
// Variable-length integers
// ---------------------------------------------------------------------------

TEST(BitPack, VarUIntRoundTrips) {
  const uint64_t values[] = {0u,      1u,        15u,        16u,
                             255u,    256u,      65535u,     1000000u,
                             UINT32_MAX, UINT64_MAX};
  for (uint64_t value : values) {
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteVarUInt(value);
    }
    BitReader bits(*buffer);
    ASSERT_EQ(bits.ReadVarUInt(), value) << "value " << value;
    ASSERT_TRUE(bits.ok());
  }
}

TEST(BitPack, VarUIntCostsFiveBitsForSmallValues) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteVarUInt(9);
    EXPECT_EQ(bits.bits_written(), 5u);
    bits.WriteVarUInt(200);
    EXPECT_EQ(bits.bits_written(), 15u);  // 5 + 10
  }
}

TEST(BitPack, VarUIntRandomRoundTrip) {
  std::mt19937_64 rng(5u);
  for (int i = 0; i < 5000; ++i) {
    const uint64_t value = rng() >> (rng() % 64);
    auto buffer = MakeBuffer();
    {
      BitWriter bits(*buffer);
      bits.WriteVarUInt(value);
    }
    BitReader bits(*buffer);
    ASSERT_EQ(bits.ReadVarUInt(), value);
  }
}

// ---------------------------------------------------------------------------
// Alignment and interleaving with ordinary Buffer fields
// ---------------------------------------------------------------------------

TEST(BitPack, AlignAdvancesToAByteBoundary) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0x5u, 3);
    bits.Align();
    EXPECT_EQ(bits.bits_written(), 8u);
    bits.WriteBits(0xFFu, 8);
    EXPECT_EQ(bits.bits_written(), 16u);
  }
  EXPECT_EQ(buffer->size(), 2u);

  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadBits(3), 0x5u);
  bits.Align();
  EXPECT_EQ(bits.ReadBits(8), 0xFFu);
}

TEST(BitPack, AlignOnABoundaryIsANoOp) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0xFFu, 8);
    bits.Align();
    EXPECT_EQ(bits.bits_written(), 8u);
  }
  EXPECT_EQ(buffer->size(), 1u);
}

// the point of building on Buffer rather than replacing it: one packet can mix
// bit fields with ordinary Buffer fields, and the cursors stay consistent.
TEST(BitPack, InterleavesWithOrdinaryBufferFields) {
  auto buffer = MakeBuffer();
  buffer->WriteString("player");
  buffer->WriteInt<uint32_t>(0xDEADBEEFu);
  {
    BitWriter bits(*buffer);
    bits.WriteBool(true);
    bits.WriteUIntRanged(1234, 0, 4095);
  }
  buffer->WriteFloat(2.5f);
  buffer->WriteString("tail");

  EXPECT_EQ(buffer->ReadString(), "player");
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 0xDEADBEEFu);
  {
    BitReader bits(*buffer);
    EXPECT_TRUE(bits.ReadBool());
    EXPECT_EQ(bits.ReadUIntRanged(0, 4095), 1234u);
  }
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 2.5f);
  EXPECT_EQ(buffer->ReadString(), "tail");
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::None);
}

// several independent bit runs in one packet, each byte-aligned by its own
// flush, must not bleed into each other.
TEST(BitPack, SeparateBitRunsDoNotBleed) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0x7u, 3);
  }
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0x7u, 3);
  }
  EXPECT_EQ(buffer->size(), 2u);

  {
    BitReader bits(*buffer);
    EXPECT_EQ(bits.ReadBits(3), 0x7u);
    bits.Align();
  }
  {
    BitReader bits(*buffer);
    EXPECT_EQ(bits.ReadBits(3), 0x7u);
  }
}

TEST(BitPack, FlushIsIdempotentAndWritingMayContinue) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0x5u, 3);
    bits.Flush();
    bits.Flush();
    EXPECT_EQ(buffer->size(), 1u);
    bits.WriteBits(0x2u, 2);
    bits.Flush();
    EXPECT_EQ(buffer->size(), 2u);
  }
  EXPECT_EQ(buffer->size(), 2u);
}

// ---------------------------------------------------------------------------
// Truncated and hostile input
// ---------------------------------------------------------------------------

TEST(BitPack, ReadingPastTheEndFailsQuietlyAndReportsIt) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0x5u, 3);
  }
  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadBits(3), 0x5u);
  EXPECT_TRUE(bits.ok());

  EXPECT_EQ(bits.ReadBits(32), 0u);  // only 5 padding bits remain
  EXPECT_FALSE(bits.ok());
  // and it lands on the buffer too, so a per-packet error check catches it
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::ReadOutOfBounds);
}

TEST(BitPack, ReadsAfterFailureKeepReturningZero) {
  auto buffer = MakeBuffer();
  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadBits(8), 0u);
  EXPECT_FALSE(bits.ok());
  EXPECT_EQ(bits.ReadBits(8), 0u);
  EXPECT_EQ(bits.ReadUIntRanged(10, 20), 10u);
  EXPECT_EQ(bits.ReadVarUInt(), 0u);
  EXPECT_FALSE(bits.ok());
}

// a truncated stream must not be able to make ReadVarUInt spin.
TEST(BitPack, VarUIntTerminatesOnTruncatedInput) {
  auto buffer = MakeBuffer();
  // every group says "more follows", and the bytes run out
  {
    BitWriter bits(*buffer);
    for (int i = 0; i < 4; ++i) {
      bits.WriteBits(0xFu, 4);
      bits.WriteBool(true);
    }
  }
  BitReader bits(*buffer);
  bits.ReadVarUInt();  // must return, not hang
  SUCCEED();
}

// same, but with a well-formed stream of nothing but continuation groups: the
// 16-group ceiling has to stop it.
TEST(BitPack, VarUIntStopsAtSixteenGroups) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    for (int i = 0; i < 40; ++i) {
      bits.WriteBits(0xFu, 4);
      bits.WriteBool(true);
    }
  }
  BitReader bits(*buffer);
  bits.ReadVarUInt();
  EXPECT_LE(bits.bits_read(), 16u * 5u);
}

TEST(BitPack, ArbitraryBytesDecodeWithoutTrouble) {
  std::mt19937 rng(77u);
  for (int trial = 0; trial < 2000; ++trial) {
    auto buffer = MakeBuffer();
    const unsigned length = 1 + (rng() % 16);
    for (unsigned i = 0; i < length; ++i) {
      buffer->WriteInt<uint8_t>(static_cast<uint8_t>(rng()));
    }
    BitReader bits(*buffer);
    for (int field = 0; field < 10; ++field) {
      const uint64_t ranged = bits.ReadUIntRanged(100, 500);
      ASSERT_GE(ranged, 100u);
      ASSERT_LE(ranged, 500u);
      const float f = bits.ReadFloatRanged(-1.0f, 1.0f, 9);
      ASSERT_TRUE(std::isfinite(f));
      ASSERT_GE(f, -1.0f);
      ASSERT_LE(f, 1.0f);
      bits.ReadVarUInt();
    }
  }
}

TEST(BitPack, RemainingBitsTracksTheBuffer) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0u, 24);
  }
  BitReader bits(*buffer);
  EXPECT_EQ(bits.remaining_bits(), 24u);
  bits.ReadBits(3);
  EXPECT_EQ(bits.remaining_bits(), 21u);
  bits.ReadBits(21);
  EXPECT_EQ(bits.remaining_bits(), 0u);
}

// ---------------------------------------------------------------------------
// Caller mistakes
// ---------------------------------------------------------------------------

TEST(BitPack, OverWideFieldIsFlaggedNotUndefined) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0xFFFFFFFFu, 33);  // wider than a single call allows
    EXPECT_FALSE(bits.ok());
  }
  SUCCEED();
}

TEST(BitPack, ZeroWidthFieldsCostNothing) {
  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    bits.WriteBits(0xFFu, 0);
    bits.WriteFloatRanged(1.0f, 0.0f, 2.0f, 0);
    EXPECT_EQ(bits.bits_written(), 0u);
  }
  EXPECT_EQ(buffer->size(), 0u);

  BitReader bits(*buffer);
  EXPECT_EQ(bits.ReadBits(0), 0u);
  EXPECT_TRUE(bits.ok());
}
