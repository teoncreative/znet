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
#include "znet/test_tools.h"
#include "znet/znet.h"

using namespace znet;

void TestBuffer(Ref<Buffer> buffer, int test_no) {
  int64_t asd = INT64_MAX;
  float f = 0.9245f;
  double d = 0.224529726;
  buffer->ReserveExact(80);
  buffer->WriteString("Hello World!");
  buffer->WriteInt(asd);
  buffer->WriteInt(f);
  buffer->WriteInt(d);

  std::cout << buffer->Dump() << std::endl;
  MATCH_AND_EXIT_A(buffer->ReadString(), "Hello World!")
  MATCH_AND_EXIT_A(buffer->ReadInt<int64_t>(), asd)
  MATCH_AND_EXIT_A(buffer->ReadInt<float>(), f)
  MATCH_AND_EXIT_A(buffer->ReadInt<double>(), d)
  MATCH_AND_EXIT_A(buffer->mem_allocations(), 1)
  MATCH_AND_EXIT_A(buffer->size(), 40)

  MATCH_AND_EXIT_A(buffer->capacity(), 80)
  buffer->Trim();
  MATCH_AND_EXIT_A(buffer->capacity(), 40)

  ZNET_LOG_INFO("size: {}", buffer->size());
  ZNET_LOG_INFO("capacity: {}", buffer->capacity());
  ZNET_LOG_INFO("mem_allocations: {}", buffer->mem_allocations());
  ZNET_LOG_INFO("buffer test {} passed!", test_no);
}

void TestVarInt(Ref<Buffer> buffer, int test_no) {
  int64_t n1 = INT64_MAX;
  int64_t n2 = 124;
  int64_t n3 = 258;

  buffer->WriteVarInt(n1);
  buffer->WriteVarInt(n2);
  buffer->WriteVarInt(n3);

  std::cout << buffer->Dump() << std::endl;
  MATCH_AND_EXIT_A(buffer->ReadVarInt<int64_t>(), n1)
  MATCH_AND_EXIT_A(buffer->ReadVarInt<int64_t>(), n2)
  MATCH_AND_EXIT_A(buffer->ReadVarInt<int64_t>(), n3)

  ZNET_LOG_INFO("size: {}", buffer->size());
  ZNET_LOG_INFO("var-int test {} passed!", test_no);
}

int main() {
  auto buffer = CreateRef<Buffer>(Endianness::LittleEndian);
  TestBuffer(buffer, 1);
  buffer->Reset(true);
  TestVarInt(buffer, 1);

  buffer = CreateRef<Buffer>(Endianness::BigEndian);
  TestBuffer(buffer, 2);
  buffer->Reset(true);
  TestVarInt(buffer, 2);
}
