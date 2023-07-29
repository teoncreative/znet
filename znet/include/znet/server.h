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

#include "base/interface.h"
#include "base/server_session.h"
#include "logger.h"

namespace znet {

struct ServerConfig {
  std::string bind_ip_;
  int bind_port_;
};

class Server : public Interface {
 public:
  Server();
  Server(const ServerConfig& config);

  ~Server();

  void Bind() override;
  void Listen();
  void Stop();

  ZNET_NODISCARD bool shutdown_complete() const { return shutdown_complete_; }

 private:
  void CheckNetwork();
  void ProcessSessions();

 private:
  ServerConfig config_;
  Ref<InetAddress> bind_address_;
  bool is_listening_ = false;
  SocketType server_socket_ = -1;
  bool shutdown_complete_ = false;

  std::unordered_map<Ref<InetAddress>, Ref<ServerSession>> sessions_;
};
}  // namespace znet