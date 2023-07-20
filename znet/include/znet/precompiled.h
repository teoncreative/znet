//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <string>
#include <any>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <iostream>
#include <sstream>

#ifdef __APPLE__
#define TARGET_APPLE
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#endif

#define ZNET_NODISCARD [[nodiscard]]
