//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/error.h"

std::string GetLastErrorInfo() {
#ifdef TARGET_WIN
  char buf[256];
  buf[0] = '\0';
  int err = WSAGetLastError();
  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                err,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                buf,
                sizeof(buf),
                nullptr);
  return {buf};
#else
  // glibc ships two signatures for strerror_r:
  //   * XSI:  int  strerror_r(int, char*, size_t)  - fills buf, returns 0/errno
  //   * GNU:  char* strerror_r(int, char*, size_t) - may return a static string
  //     WITHOUT touching buf. Using buf directly prints uninitialized stack
  //     bytes (the "garbage on failure" symptom). Dispatch at compile time.
  int saved_errno = errno;
  char buf[256];
  buf[0] = '\0';
#if ((_POSIX_C_SOURCE >= 200112L) && !_GNU_SOURCE)
  strerror_r(saved_errno, buf, sizeof(buf));
  return {buf};
#else
  const char* msg = strerror_r(saved_errno, buf, sizeof(buf));
  return std::string(msg ? msg : "unknown error");
#endif
#endif
}
