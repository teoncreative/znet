//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// windows.h, reached through sys_net.h so winsock2 is always first. Empty on
// every other platform, which is what lets a .cc include it unconditionally.
//
// Only the handful of sources that call the wider Win32 API (console setup,
// FormatMessage) need this; socket code wants znet/detail/sys_net.h instead.
//

#ifndef ZNET_DETAIL_SYS_WIN_H_
#define ZNET_DETAIL_SYS_WIN_H_

#include "znet/detail/sys_net.h"

#if defined(ZNET_TARGET_WIN)
#include <windows.h>
#endif

#endif  // ZNET_DETAIL_SYS_WIN_H_
