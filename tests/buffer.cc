//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#define ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS

#include "znet/buffer.h"
#include "gtest/gtest.h"

using namespace znet;

class BufferTest : public ::testing::Test {
 protected:
  BufferTest() {
    buffer_le_ = std::make_shared<Buffer>(Endianness::LittleEndian);
    buffer_be_ = std::make_shared<Buffer>(Endianness::BigEndian);
  }

  virtual ~BufferTest() {
  }

  virtual void SetUp() {
    buffer_le_->Reset(true);
    buffer_be_->Reset(true);
  }

  virtual void TearDown() {
  }

  std::shared_ptr<Buffer> buffer_le_;
  std::shared_ptr<Buffer> buffer_be_;
};

// todo split these to individual tests
void TestBuffer(std::shared_ptr<Buffer> buffer) {
  EXPECT_EQ(buffer->size(), 0);

  int64_t asd = INT64_MAX;
  float f = 0.9245f;
  double d = 0.224529726;
  buffer->ReserveExact(80);
  buffer->WriteString("Hello World!");
  buffer->WriteInt(asd);
  buffer->WriteFloat(f);
  buffer->WriteDouble(d);
  buffer->WriteDouble(d);

  std::cout << buffer->Dump() << std::endl;
  EXPECT_EQ(buffer->ReadString(), "Hello World!");
  EXPECT_EQ(buffer->ReadInt<int64_t>(), asd);
  EXPECT_EQ(buffer->ReadFloat(), f);
  EXPECT_EQ(buffer->ReadDouble(), d);
  EXPECT_EQ(buffer->ReadDouble(), d);
  EXPECT_EQ(buffer->mem_allocations(), 1);
  EXPECT_EQ(buffer->size(), 42);

  EXPECT_EQ(buffer->capacity(), 80);
  buffer->Trim();
  EXPECT_EQ(buffer->capacity(), 42);

  EXPECT_EQ(buffer->size(), 42);
  EXPECT_EQ(buffer->capacity(), 42);
  EXPECT_EQ(buffer->mem_allocations(), 1);
}


void TestVarInt(std::shared_ptr<Buffer> buffer) {
  EXPECT_EQ(buffer->size(), 0);

  int64_t n1 = INT64_MAX;
  int64_t n2 = 124;
  int64_t n3 = 258;

  buffer->WriteVarInt(n1);
  buffer->WriteVarInt(n2);
  buffer->WriteVarInt(n3);

  std::cout << buffer->Dump() << std::endl;
  EXPECT_EQ(buffer->ReadVarInt<int64_t>(), n1);
  EXPECT_EQ(buffer->ReadVarInt<int64_t>(), n2);
  EXPECT_EQ(buffer->ReadVarInt<int64_t>(), n3);
  EXPECT_EQ(buffer->size(), 14);
}

void TestInetAddress(std::shared_ptr<Buffer> buffer) {
  EXPECT_EQ(buffer->size(), 0);

  auto addr1 = InetAddress::from("127.0.0.1", 2001);
  buffer->WriteInetAddress(*addr1);

  auto addr2 = InetAddress::from("2001:db8:3333:4444:5555:6666:7777:8888", 2001);
  buffer->WriteInetAddress(*addr2);

  std::cout << buffer->Dump() << std::endl;
  EXPECT_EQ(*buffer->ReadInetAddress(), *addr1);
  EXPECT_EQ(*buffer->ReadInetAddress(), *addr2);

  EXPECT_EQ(buffer->size(), 26);
}

// ReadString used to allocate a scratch array, fill it a character at a time,
// construct the string from it and then leak the scratch. It now copies
// straight out of the buffer, so these cases guard the cases that a
// byte-at-a-time loop got right by accident: an empty string, a string with an
// embedded NUL, and non-ASCII bytes that must not be sign-extended or treated
// as a terminator.
void TestStringRoundTrip(std::shared_ptr<Buffer> buffer) {
  const std::string cases[] = {
      "",
      "a",
      "Hello World!",
      std::string("with\0embedded\0nuls", 18),
      std::string("\x01\x02\xfe\xff", 4),
      "unicode: \xc3\xa9\xe2\x82\xac",
      std::string(4096, 'x'),
  };

  for (const std::string& s : cases) {
    buffer->WriteString(s);
  }
  for (const std::string& s : cases) {
    const std::string got = buffer->ReadString();
    EXPECT_EQ(got, s);
    EXPECT_EQ(got.size(), s.size());
  }
  EXPECT_EQ(buffer->readable_bytes(), 0u);
  EXPECT_EQ(buffer->GetAndClearLastError(), BufferError::None);
}

TEST_F(BufferTest, TestBuffers) {
  TestBuffer(buffer_le_);
  TestBuffer(buffer_be_);
}

TEST_F(BufferTest, TestStringRoundTrip) {
  TestStringRoundTrip(buffer_le_);
  TestStringRoundTrip(buffer_be_);
}

TEST_F(BufferTest, TestVarInts) {
  TestVarInt(buffer_le_);
  TestVarInt(buffer_be_);
}

TEST_F(BufferTest, TestInetAddress) {
  TestInetAddress(buffer_le_);
  TestInetAddress(buffer_be_);
}