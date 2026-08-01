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

#include "znet/ext/glm/serialize.h"

using znet::Buffer;
using znet::BufferError;
using znet::Endianness;

namespace ext = znet::ext;

namespace {

std::shared_ptr<Buffer> MakeBuffer(
    Endianness endianness = Endianness::LittleEndian) {
  return std::make_shared<Buffer>(endianness);
}

}  // namespace

TEST(GlmSerialize, Vec3RoundTrip) {
  auto buffer = MakeBuffer();
  const glm::vec3 value(1.5f, -2.25f, 3.125f);
  ext::WriteVec(*buffer, value);

  EXPECT_EQ(buffer->size(), sizeof(float) * 3);

  const auto read = ext::ReadVec<glm::vec3>(*buffer);
  EXPECT_EQ(read, value);
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::None);
}

TEST(GlmSerialize, EveryVectorArity) {
  auto buffer = MakeBuffer();
  const glm::vec2 v2(1.0f, 2.0f);
  const glm::vec3 v3(3.0f, 4.0f, 5.0f);
  const glm::vec4 v4(6.0f, 7.0f, 8.0f, 9.0f);
  ext::WriteVec(*buffer, v2);
  ext::WriteVec(*buffer, v3);
  ext::WriteVec(*buffer, v4);

  EXPECT_EQ(ext::ReadVec<glm::vec2>(*buffer), v2);
  EXPECT_EQ(ext::ReadVec<glm::vec3>(*buffer), v3);
  EXPECT_EQ(ext::ReadVec<glm::vec4>(*buffer), v4);
}

TEST(GlmSerialize, ComponentTypes) {
  auto buffer = MakeBuffer();
  const glm::ivec3 i3(-1, 2, -3);
  const glm::uvec2 u2(7u, 8u);
  const glm::dvec4 d4(1.0, 2.0, 3.0, 4.0);
  const glm::i16vec2 s2(static_cast<int16_t>(-300), static_cast<int16_t>(300));

  ext::WriteVec(*buffer, i3);
  ext::WriteVec(*buffer, u2);
  ext::WriteVec(*buffer, d4);
  ext::WriteVec(*buffer, s2);

  const size_t expected =
      sizeof(int) * 3 + sizeof(unsigned) * 2 + sizeof(double) * 4 + 2 * 2;
  EXPECT_EQ(buffer->size(), expected);

  EXPECT_EQ(ext::ReadVec<glm::ivec3>(*buffer), i3);
  EXPECT_EQ(ext::ReadVec<glm::uvec2>(*buffer), u2);
  EXPECT_EQ(ext::ReadVec<glm::dvec4>(*buffer), d4);
  EXPECT_EQ(ext::ReadVec<glm::i16vec2>(*buffer), s2);
}

// bool components are normalised through an integer rather than memcpy'd, so
// a byte that is neither 0 nor 1 still produces a usable bool.
TEST(GlmSerialize, BoolVectorRoundTrip) {
  auto buffer = MakeBuffer();
  const glm::bvec3 value(true, false, true);
  ext::WriteVec(*buffer, value);

  EXPECT_EQ(buffer->size(), 3u);
  EXPECT_EQ(ext::ReadVec<glm::bvec3>(*buffer), value);
}

TEST(GlmSerialize, BoolVectorToleratesNonCanonicalBytes) {
  auto buffer = MakeBuffer();
  buffer->WriteInt<uint8_t>(0xFFu);
  buffer->WriteInt<uint8_t>(0x00u);
  buffer->WriteInt<uint8_t>(0x02u);

  const auto read = ext::ReadVec<glm::bvec3>(*buffer);
  EXPECT_TRUE(read.x);
  EXPECT_FALSE(read.y);
  EXPECT_TRUE(read.z);
  // the point of the normalisation: `b` and `!b` must not both be true
  EXPECT_NE(read.z, !read.z);
}

TEST(GlmSerialize, Mat4RoundTrip) {
  auto buffer = MakeBuffer();
  glm::mat4 value(1.0f);
  for (glm::length_t c = 0; c < 4; ++c) {
    for (glm::length_t r = 0; r < 4; ++r) {
      value[c][r] = static_cast<float>(c * 4 + r) + 0.5f;
    }
  }
  ext::WriteMat(*buffer, value);

  EXPECT_EQ(buffer->size(), sizeof(float) * 16);
  EXPECT_EQ(ext::ReadMat<glm::mat4>(*buffer), value);
}

TEST(GlmSerialize, NonSquareMatRoundTrip) {
  auto buffer = MakeBuffer();
  glm::mat2x3 value(0.0f);
  value[0] = glm::vec3(1.0f, 2.0f, 3.0f);
  value[1] = glm::vec3(4.0f, 5.0f, 6.0f);
  ext::WriteMat(*buffer, value);

  EXPECT_EQ(buffer->size(), sizeof(float) * 6);
  EXPECT_EQ(ext::ReadMat<glm::mat2x3>(*buffer), value);
}

// column-major, matching glm's own storage: the first four floats on the wire
// are column 0, not row 0.
TEST(GlmSerialize, MatIsColumnMajorOnTheWire) {
  auto buffer = MakeBuffer();
  glm::mat4 value(0.0f);
  value[0] = glm::vec4(1.0f, 2.0f, 3.0f, 4.0f);
  ext::WriteMat(*buffer, value);

  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 1.0f);
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 2.0f);
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 3.0f);
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 4.0f);
}

TEST(GlmSerialize, QuatRoundTrip) {
  auto buffer = MakeBuffer();
  const glm::quat value =
      glm::normalize(glm::quat(0.5f, 0.1f, -0.2f, 0.83f));
  ext::WriteQuat(*buffer, value);

  EXPECT_EQ(buffer->size(), sizeof(float) * 4);
  const auto read = ext::ReadQuat<glm::quat>(*buffer);
  EXPECT_FLOAT_EQ(read.x, value.x);
  EXPECT_FLOAT_EQ(read.y, value.y);
  EXPECT_FLOAT_EQ(read.z, value.z);
  EXPECT_FLOAT_EQ(read.w, value.w);
}

// the wire order is x, y, z, w by name. This must hold whatever
// GLM_FORCE_QUAT_DATA_* the consumer built glm with, so the test reads the
// floats back positionally rather than through glm.
TEST(GlmSerialize, QuatWireOrderIsXyzw) {
  auto buffer = MakeBuffer();
  glm::quat value(4.0f, 1.0f, 2.0f, 3.0f);  // glm::quat(w, x, y, z)
  ext::WriteQuat(*buffer, value);

  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 1.0f);  // x
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 2.0f);  // y
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 3.0f);  // z
  EXPECT_FLOAT_EQ(buffer->ReadFloat(), 4.0f);  // w
}

TEST(GlmSerialize, BigEndianBufferRoundTrips) {
  auto buffer = MakeBuffer(Endianness::BigEndian);
  const glm::vec3 value(1.5f, -2.25f, 3.125f);
  ext::WriteVec(*buffer, value);
  EXPECT_EQ(ext::ReadVec<glm::vec3>(*buffer), value);
}

TEST(GlmSerialize, VecArrayRoundTrip) {
  auto buffer = MakeBuffer();
  std::vector<glm::vec3> value;
  for (int i = 0; i < 32; ++i) {
    const auto f = static_cast<float>(i);
    value.push_back(glm::vec3(f, f * 2.0f, f * 3.0f));
  }
  ext::WriteVecArray(*buffer, value);

  std::vector<glm::vec3> read;
  ASSERT_TRUE(ext::ReadVecArray(*buffer, read));
  EXPECT_EQ(read, value);
}

TEST(GlmSerialize, EmptyVecArrayRoundTrip) {
  auto buffer = MakeBuffer();
  const std::vector<glm::vec2> value;
  ext::WriteVecArray(*buffer, value);

  std::vector<glm::vec2> read;
  ASSERT_TRUE(ext::ReadVecArray(*buffer, read));
  EXPECT_TRUE(read.empty());
}

// a hostile count must not become a hostile allocation: the element bytes have
// to already be in the buffer before anything is reserved.
TEST(GlmSerialize, VecArrayRejectsImplausibleCount) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(4000000000u));
  buffer->WriteFloat(1.0f);

  std::vector<glm::vec3> read;
  EXPECT_FALSE(ext::ReadVecArray(*buffer, read));
  EXPECT_TRUE(read.empty());
}

TEST(GlmSerialize, VecArrayRejectsTruncatedPayload) {
  auto buffer = MakeBuffer();
  std::vector<glm::vec3> value(4, glm::vec3(1.0f));
  ext::WriteVecArray(*buffer, value);
  // drop the final vec3 from what the reader can see
  buffer->SetReadLimit(buffer->size() - sizeof(float) * 3);

  std::vector<glm::vec3> read;
  EXPECT_FALSE(ext::ReadVecArray(*buffer, read));
  EXPECT_TRUE(read.empty());
}

// reads past the end report through the buffer, the same as the core Read*
// methods, rather than through a return value on every field.
TEST(GlmSerialize, TruncatedReadSetsBufferError) {
  auto buffer = MakeBuffer();
  buffer->WriteFloat(1.0f);  // one component of a vec3

  const auto read = ext::ReadVec<glm::vec3>(*buffer);
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::ReadOutOfBounds);
  EXPECT_FLOAT_EQ(read.x, 1.0f);
  EXPECT_FLOAT_EQ(read.y, 0.0f);
  EXPECT_FLOAT_EQ(read.z, 0.0f);
}

// the extension is a plain Buffer consumer, so it interleaves with the core
// writers inside one packet body.
TEST(GlmSerialize, InterleavesWithCoreBufferWrites) {
  auto buffer = MakeBuffer();
  buffer->WriteString("entity");
  ext::WriteVec(*buffer, glm::vec3(1.0f, 2.0f, 3.0f));
  buffer->WriteInt<uint32_t>(42u);
  ext::WriteQuat(*buffer, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

  EXPECT_EQ(buffer->ReadString(), "entity");
  EXPECT_EQ(ext::ReadVec<glm::vec3>(*buffer), glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 42u);
  EXPECT_FLOAT_EQ(ext::ReadQuat<glm::quat>(*buffer).w, 1.0f);
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::None);
}
