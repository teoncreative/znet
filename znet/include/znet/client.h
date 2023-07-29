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

#include "base/client_session.h"
#include "base/interface.h"

namespace znet {

struct ClientConfig {
  std::string server_ip_;
  int server_port_;
};

class Client : public Interface {
 public:
  Client(const ClientConfig& config);

  ~Client();

  void Bind() override;
  bool Connect();
  void Disconnect();

  Ref<ClientSession> client_session() const { return client_session_; }
 private:
  ClientConfig config_;
  Ref<InetAddress> server_address_;
  SocketType client_socket_ = -1;

  Ref<ClientSession> client_session_;
};

}  // namespace znet