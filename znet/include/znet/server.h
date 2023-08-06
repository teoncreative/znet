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
#include "znet/base/scheduler.h"

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

  Result Bind() override;
  Result Listen();
  Result Stop();

  void SetTicksPerSecond(int tps);

  ZNET_NODISCARD bool shutdown_complete() const { return shutdown_complete_; }
  ZNET_NODISCARD int tps() const { return tps_; }

 private:
  void CheckNetwork();
  void ProcessSessions();

 private:
  std::mutex mutex_;
  ServerConfig config_;
  Ref<InetAddress> bind_address_;
  bool is_listening_ = false;
  SocketType server_socket_ = -1;
  bool shutdown_complete_ = false;
  int tps_ = 120;
  Scheduler scheduler_{tps_};

  std::unordered_map<Ref<InetAddress>, Ref<ServerSession>> sessions_;
};
}  // namespace znet