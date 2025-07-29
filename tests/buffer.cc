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
#include "znet/znet.h"

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

// todo split these to more individuaş
void TestBuffer(std::shared_ptr<Buffer> buffer) {
    int64_t asd = INT64_MAX;
    float f = 0.9245f;
    double d = 0.224529726;
    buffer->ReserveExact(80);
    buffer->WriteString("Hello World!");
    buffer->WriteInt(asd);
    buffer->WriteInt(f);
    buffer->WriteInt(d);
    buffer->WriteInt(d);

    std::cout << buffer->Dump() << std::endl;
    EXPECT_EQ(buffer->ReadString(), "Hello World!");
    EXPECT_EQ(buffer->ReadInt<int64_t>(), asd);
    EXPECT_EQ(buffer->ReadInt<float>(), f);
    EXPECT_EQ(buffer->ReadInt<double>(), d);
    EXPECT_EQ(buffer->ReadInt<double>(), d);
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

TEST_F(BufferTest, TestBuffers) {
  TestBuffer(buffer_le_);
  TestBuffer(buffer_be_);
}

TEST_F(BufferTest, TestVarInts) {
  TestVarInt(buffer_le_);
  TestVarInt(buffer_be_);
}