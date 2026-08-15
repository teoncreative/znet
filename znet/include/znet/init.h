//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_INIT_H_
#define ZNET_INIT_H_

#include "znet/compat.h"
#include "znet/types.h"

namespace znet {

/**
 * @brief Global library setup: sockets (WSAStartup on Windows) and feature
 *        registration. Call once before anything else; further calls are
 *        cheap no-ops, so components may call it defensively.
 */
Result Init();

/** @brief Releases what Init() acquired. Call after the last znet object died. */
void Cleanup();

}  // namespace znet

#endif  // ZNET_INIT_H_
