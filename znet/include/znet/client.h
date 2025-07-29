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

#include "precompiled.h"
#include "interface.h"
#include "peer_session.h"
#include "task.h"

namespace znet {

struct ClientConfig {
  std::string server_ip;
  int server_port;
  ConnectionType connection_type;
};

class Client : public Interface {
 public:
  Client(const ClientConfig& config);
  Client(const Client&) = delete;

  ~Client();

  Result Bind() override;
  void Wait() override;
  Result Connect();
  Result Disconnect();

  ZNET_NODISCARD std::shared_ptr<PeerSession> client_session() const {
    return client_session_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> server_address() const {
    return server_address_;
  }

 private:
  ClientConfig config_;
  std::shared_ptr<InetAddress> server_address_;
  SocketHandle client_socket_ = -1;

  std::shared_ptr<PeerSession> client_session_;
  Task task_;

};

}  // namespace znet