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

#include "logger.h"
#include "base/interface.h"
#include "base/server_session.h"

namespace znet {

  struct ServerConfig {
    std::string bind_ip_;
    int bind_port_;
  };


  class Server : public Interface {
  public:
    Server(const ServerConfig& config) : Interface(), config_(config) { }
    ~Server() { }

    void Bind();
    void Listen();
    void Stop();


  private:
    void CheckNetwork();
    void ProcessSessions();

  private:
    ServerConfig config_;
    Ref<InetAddress> bind_address_;
    bool is_listening_ = false;
    int server_socket_ = -1;

    std::unordered_map<Ref<InetAddress>, Ref<ServerSession>> sessions_;
  };
}