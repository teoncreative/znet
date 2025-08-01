//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/dialer.h"

namespace znet {
namespace holepunch {

Dialer::Dialer(const DialerConfig& config) {
  client_ = std::make_unique<Client>(ClientConfig{config.peer_ext_ip, config.local_port});
  server_ = std::make_unique<Server>(ServerConfig{config.local_ip, config.local_port});
}

Dialer::~Dialer() {

}

Result Dialer::Bind() {
  Result result;
  if ((result = client_->Bind()) != Result::Success) {
    return result;
  }
  if ((result = server_->Bind()) != Result::Success) {
    return result;
  }
  return Result::Success;
}

Result Dialer::Dial() {
  Result result;
  if ((result = client_->Connect()) != Result::Success) {
    return result;
  }
  if ((result = server_->Listen()) != Result::Success) {
    return result;
  }
  return Result::Success;
}

void Dialer::Wait() {
  client_->Wait();
  server_->Wait();
}

}
}