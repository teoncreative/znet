//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "packets.h"
#include "chat_server.h"
#include "user.h"

using namespace znet;

int main() {
  ChatServer server{};
  server.Start();
}