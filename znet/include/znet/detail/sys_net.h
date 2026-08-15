//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_DETAIL_SYS_NET_H_
#define ZNET_DETAIL_SYS_NET_H_

#include "znet/detail/platform.h"

#if defined(ZNET_TARGET_WIN)

// windows.h pulls in the incompatible winsock 1 unless it is told not to, and
// it is only ever a problem when it got there first.
#if defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#error \
    "windows.h was included before znet and brought winsock 1 with it. Define WIN32_LEAN_AND_MEAN, or include znet before windows.h."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

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

#else  // ZNET_TARGET_POSIX

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if ZNET_HAS_AF_UNIX
#include <sys/un.h>
#endif

#endif

#endif  // ZNET_DETAIL_SYS_NET_H_
