//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

// The ceilings are documented as optional: 0 turns one off for a trusted link
// where a legitimate message really is larger than the default. That leaves the
// bytes-on-hand check alone against a hostile length, which is the whole claim
// worth testing, and it cannot be tested in the same translation unit as the
// ceilings themselves.
#undef ZNET_MAX_READ_ELEMENTS
#define ZNET_MAX_READ_ELEMENTS 0
#undef ZNET_MAX_READ_STRING_LENGTH
#define ZNET_MAX_READ_STRING_LENGTH 0

#include "znet/buffer.h"
#include "gtest/gtest.h"

using namespace znet;

static std::shared_ptr<Buffer> ClaimingCount(size_t count) {
  auto buffer = std::make_shared<Buffer>(Endianness::LittleEndian);
  buffer->WriteVarInt(count);
  return buffer;
}

TEST(BufferNoReadLimits, StillRefusesACountTheBytesCannotBack) {
  const size_t absurd = size_t{1} << 40;

  auto vec = ClaimingCount(absurd);
  EXPECT_TRUE(vec->ReadVector<char>(&Buffer::ReadChar).empty());
  EXPECT_EQ(vec->GetAndClearLastError(), BufferError::ReadOutOfBounds);

  auto map = ClaimingCount(absurd);
  EXPECT_TRUE(
      (map->ReadMap<std::map<char, char>>(&Buffer::ReadChar, &Buffer::ReadChar))
          .empty());
  EXPECT_EQ(map->GetAndClearLastError(), BufferError::ReadOutOfBounds);

  auto arr = ClaimingCount(absurd);
  EXPECT_EQ(arr->ReadArray<char>(&Buffer::ReadChar), nullptr);
  EXPECT_EQ(arr->GetAndClearLastError(), BufferError::ReadOutOfBounds);

  auto str = ClaimingCount(absurd);
  EXPECT_EQ(str->ReadString(), "");
  EXPECT_EQ(str->GetAndClearLastError(), BufferError::ReadOutOfBounds);
}

// count * sizeof(T) is the product an attacker picks to wrap: past the wrap the
// byte total looks small, the check passes, and new T[count] is reached with a
// count nothing bounds. only reachable with the ceiling off, which is exactly
// the configuration under test.
TEST(BufferNoReadLimits, ReadArrayRefusesACountThatWouldOverflowItsByteSize) {
  const size_t wraps =
      (std::numeric_limits<size_t>::max() / sizeof(int64_t)) + 2;
  ASSERT_EQ(wraps * sizeof(int64_t), sizeof(int64_t));
  auto buffer = ClaimingCount(wraps);
  // the wrapped total is one element wide, so a check written as a
  // multiplication finds the bytes it asks for and goes on to allocate
  buffer->WriteInt<int64_t>(0);
  EXPECT_EQ(buffer->ReadArray<int64_t>(&Buffer::ReadInt<int64_t>), nullptr);
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::ReadOutOfBounds);
}

// nothing about turning the ceilings off may change what a well-formed message
// reads back as
TEST(BufferNoReadLimits, RoundTripsWhatTheBytesActuallyBack) {
  auto buffer = std::make_shared<Buffer>(Endianness::LittleEndian);
  std::vector<char> v(4096, 'z');
  buffer->WriteVector(v, &Buffer::WriteChar);
  buffer->WriteString(std::string(4096, 'q'));

  EXPECT_EQ(buffer->ReadVector<char>(&Buffer::ReadChar), v);
  EXPECT_EQ(buffer->ReadString().size(), 4096u);
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::None);
}
