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

#include "client_session.h"
#include "interface.h"

namespace znet {

struct ClientConfig {
  std::string server_ip_;
  int server_port_;
};

class Client : public Interface {
 public:
  Client(const ClientConfig& config);
  Client(const Client&) = delete;

  ~Client();

  Result Bind() override;
  Result Connect();
  Result Disconnect();

  ZNET_NODISCARD Ref<ClientSession> client_session() const {
    return client_session_;
  }

  ZNET_NODISCARD Ref<InetAddress> server_address() const {
    return server_address_;
  }

 private:
  ClientConfig config_;
  Ref<InetAddress> server_address_;
  SocketType client_socket_ = -1;

  Ref<ClientSession> client_session_;
};

}  // namespace znet