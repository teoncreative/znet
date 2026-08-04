//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PRECOMPILED_H_
#define ZNET_PRECOMPILED_H_

//#define ZNET_PREFER_STD_SLEEP 1
#define OPENSSL_SUPPRESS_DEPRECATED

#include "znet/compat.h"

#include <cassert>
#include <csignal>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <memory>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>

#include <fcntl.h>

#if defined(__APPLE__)
#define ZNET_TARGET_APPLE
#endif
#if defined(EMSCRIPTEN)
#define ZNET_TARGET_WEB
#endif
#if defined(__linux__)
#define ZNET_TARGET_LINUX
#endif
#if defined(ZNET_TARGET_APPLE) || defined(ZNET_TARGET_WEB) || defined(ZNET_TARGET_LINUX)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#if defined(ZNET_TARGET_LINUX)
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <cerrno>
#include <cstring>
#endif
// AF_UNIX support. POSIX only for now; Windows 10+ has it behind <afunix.h>
// and could be added here.
#if (defined(ZNET_TARGET_APPLE) || defined(ZNET_TARGET_LINUX)) && !defined(ZNET_TARGET_WEB)
#include <sys/un.h>
#define ZNET_HAS_AF_UNIX 1
#else
#define ZNET_HAS_AF_UNIX 0
#endif
#if defined(ZNET_TARGET_APPLE)
#include <netinet/tcp.h>
#endif
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define ZNET_TARGET_WIN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
// needed for the Windows 2000 IPv6 Tech Preview.
#if (_WIN32_WINNT == 0x0500)
#include <tpipv6.h>
#endif
#ifdef _MSC_VER
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

#pragma comment(lib, "Ws2_32.lib")
#endif


#endif  // ZNET_PRECOMPILED_H_
