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

#ifndef ZNET_TARGET_WIN
#include <cerrno>
#include <cstring>

namespace {

// strerror_r has two incompatible signatures - XSI returns int and fills buf,
// GNU returns the message and may leave buf untouched. Feature-test macros do
// not tell them apart reliably (macOS defines neither yet is XSI), so dispatch
// on the return type. Templates, not overloads: the unused variant is then
// never instantiated and cannot trip -Wunused-function under -Werror.

// XSI variant: the return code says whether buf was filled.
template <typename R, ZNET_ENABLE_IF(std::is_integral<R>::value)>
inline std::string StrErrorResult(R ret, const char* buf) {
  // ERANGE leaves a truncated message in buf, which still beats nothing.
  if (ret != 0 && buf[0] == '\0') {
    return "unknown error";
  }
  return {buf};
}

// GNU variant: the returned pointer is the message; buf may be untouched.
template <typename R, ZNET_ENABLE_IF(!std::is_integral<R>::value)>
inline std::string StrErrorResult(R ret, const char*) {
  return ret ? std::string(ret) : std::string("unknown error");
}

}  // namespace
#endif

namespace znet {

std::string GetLastErrorInfo() {
#ifdef ZNET_TARGET_WIN
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
  int saved_errno = errno;
  char buf[256];
  buf[0] = '\0';
  return StrErrorResult(strerror_r(saved_errno, buf, sizeof(buf)), buf);
#endif
}

}  // namespace znet
