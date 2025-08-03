//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 02/08/2025.
//

#include "znet/init.h"
#include "znet/logger.h"

namespace znet {
class SocketGuard {
 public:
  SocketGuard() {
#ifdef TARGET_WIN
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;
    /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
    wVersionRequested = MAKEWORD(2, 2);
    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
      ZNET_LOG_ERROR("WSAStartup error. {}", err);
      result_ = Result::Failure;
      return;
    }
#endif
    result_ = Result::Success;
  }

  SocketGuard(const SocketGuard&) = delete;
  SocketGuard& operator=(const SocketGuard&) = delete;

  ~SocketGuard() {}

  Result result() const { return result_; }

 private:
  bool sockets_initialized_ = false;
  Result result_;
};

static std::unique_ptr<SocketGuard> socket_guard_;

Result Init() {
  if (!socket_guard_) {
    socket_guard_ = std::make_unique<SocketGuard>();
  }
  return socket_guard_->result();
}

void Cleanup() {
  socket_guard_ = nullptr;
}

}