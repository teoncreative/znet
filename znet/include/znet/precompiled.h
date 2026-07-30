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
#define TARGET_APPLE
#endif
#if defined(EMSCRIPTEN)
#define TARGET_WEB
#endif
#if defined(__linux__)
#define TARGET_LINUX
#endif
#if defined(TARGET_APPLE) || defined(TARGET_WEB) || defined(TARGET_LINUX)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#if defined(TARGET_LINUX)
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <cerrno>
#include <cstring>
#endif
#if defined(TARGET_APPLE)
#include <netinet/tcp.h>
#endif
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define TARGET_WIN
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

// keeps a renamed type compiling under its old name. `msg` is carried for the
// day this grows a real [[deprecated]]; attaching one now would break the
// -Werror builds of anyone still on the old name.
#define DEPRECATED_TYPE_ALIAS(old, new, msg) using old = new;