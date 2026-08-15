//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_DETAIL_PLATFORM_H_
#define ZNET_DETAIL_PLATFORM_H_

#if defined(__APPLE__)
#define ZNET_TARGET_APPLE
#endif
#if defined(EMSCRIPTEN)
#define ZNET_TARGET_WEB
#endif
#if defined(__linux__)
#define ZNET_TARGET_LINUX
#endif
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define ZNET_TARGET_WIN
#endif

#if defined(ZNET_TARGET_APPLE) || defined(ZNET_TARGET_WEB) || \
    defined(ZNET_TARGET_LINUX)
#define ZNET_TARGET_POSIX
#endif

#if !defined(ZNET_TARGET_POSIX) && !defined(ZNET_TARGET_WIN)
#error "znet does not know this platform; add it to znet/detail/platform.h."
#endif

// AF_UNIX support. POSIX only for now; Windows 10+ has it behind <afunix.h>
// and could be added here.
#if defined(ZNET_TARGET_POSIX) && !defined(ZNET_TARGET_WEB)
#define ZNET_HAS_AF_UNIX 1
#else
#define ZNET_HAS_AF_UNIX 0
#endif

#endif  // ZNET_DETAIL_PLATFORM_H_
