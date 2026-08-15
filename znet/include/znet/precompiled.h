//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// The precompiled header for building znet itself, and nothing else.
//
// Nothing in znet includes this: a header that needs <winsock2.h> or
// <sstream> says so itself, so that including znet.h does not drag either into
// a consumer's translation unit. Its only job is to warm the compiler cache
// for znet's own sources, which is why it lists headers rather than declaring
// anything. Adding a header here can only make znet build faster; it can never
// make a source file compile that would not compile on its own.
//

#ifndef ZNET_PRECOMPILED_H_
#define ZNET_PRECOMPILED_H_

#include "znet/compat.h"
#include "znet/detail/platform.h"
#include "znet/detail/sys_net.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#endif  // ZNET_PRECOMPILED_H_
