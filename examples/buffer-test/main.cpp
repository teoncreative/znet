//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/znet.h"

using namespace znet;

int main() {
  auto buffer = CreateRef<Buffer>();
  int64_t asd = INT64_MAX;
  float f = 0.9245f;
  double d = 0.224529726;
  buffer->WriteString("Hello World!");
  buffer->WriteInt(asd);
  buffer->WriteInt(f);
  buffer->WriteInt(d);

  std::cout << buffer->Dump() << std::endl;
  ZNET_LOG_INFO("|{}|", buffer->ReadString());
  ZNET_LOG_INFO("|{}|", buffer->ReadInt<int64_t>());
  ZNET_LOG_INFO("|{}|", buffer->ReadInt<float>());
  ZNET_LOG_INFO("|{}|", buffer->ReadInt<double>());
  ZNET_LOG_INFO("size {}", buffer->Size());

  auto buffer2 = CreateRef<Buffer>(Endianness::BigEndian);
  buffer2->WriteString("Hello World!");
  buffer2->WriteInt(asd);
  buffer2->WriteInt(f);
  buffer2->WriteInt(d);

  std::cout << buffer2->Dump() << std::endl;
  ZNET_LOG_INFO("|{}|", buffer2->ReadString());
  ZNET_LOG_INFO("|{}|", buffer2->ReadInt<int64_t>());
  ZNET_LOG_INFO("|{}|", buffer2->ReadInt<float>());
  ZNET_LOG_INFO("size {}", buffer2->Size());
}
