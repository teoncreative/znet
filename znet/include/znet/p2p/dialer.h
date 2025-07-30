//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PARENT_DIALER_H
#define ZNET_PARENT_DIALER_H

#include "znet/interface.h"
#include "znet/client.h"
#include "znet/server.h"

namespace znet {
namespace holepunch {

enum class DialResult {
  SuccessHost,
  SuccessClient,
  FailureTimeout
};

struct DialerConfig {
  std::string local_ip;
  int local_port;
  std::string peer_ext_ip;
  ConnectionType connection_type;
};

class Dialer : public Interface {
 public:
  Dialer(const DialerConfig&);
  ~Dialer();

  Result Bind() override;

  void Wait() override;

  Result Dial();

  void SetEventCallback(EventCallbackFn fn) override {
    event_callback_ = std::move(fn);
    client_->SetEventCallback(event_callback_);
    server_->SetEventCallback(event_callback_);
  }

 private:
  std::unique_ptr<Client> client_;
  std::unique_ptr<Server> server_;
};

}
}
#endif  //ZNET_PARENT_DIALER_H
