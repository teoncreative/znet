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
  buffer->WriteString("Hello World!");
  buffer->WriteInt(asd);

  LOG_INFO("|{}|", buffer->ToStr());
  LOG_INFO("|{}|", buffer->ReadString());
  LOG_INFO("|{}|", buffer->ReadInt<int64_t>());
}
