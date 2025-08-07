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

#ifdef ZNET_USE_ZSTD
#define ZSTD_ENABLED_STR "true"
#else
#define ZSTD_ENABLED_STR "false"
#endif

namespace znet {

class Initializer {
 public:
  Initializer() {
    ZNET_LOG_INFO("Initializing znet...");
    ZNET_LOG_INFO(" - compression_zstd: {}", ZSTD_ENABLED_STR);

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

  Initializer(const Initializer&) = delete;
  Initializer& operator=(const Initializer&) = delete;

  ~Initializer() {
#ifdef TARGET_WIN
    WSACleanup();
#endif
  }

  Result result() const { return result_; }

 private:
  bool sockets_initialized_ = false;
  Result result_ = Result::NotInitialized;
};

static std::unique_ptr<Initializer> init_;

Result Init() {
  if (!init_) {
    init_ = std::make_unique<Initializer>();
  }
  return init_->result();
}

void Cleanup() {
  init_ = nullptr;
}

}