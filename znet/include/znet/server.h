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

#include "interface.h"
#include "peer_session.h"
#include "logger.h"
#include "base/scheduler.h"
#include "task.h"

namespace znet {

struct ServerConfig {
  std::string bind_ip_;
  int bind_port_;
};

class Server : public Interface {
 public:
  using SessionMap = std::unordered_map<Ref<InetAddress>, Ref<PeerSession>>;
  Server();
  Server(const ServerConfig& config);
  Server(const Server&) = delete;

  ~Server();

  Result Bind() override;
  void Wait() override;
  Result Listen();
  Result Stop();

  void SetTicksPerSecond(int tps);

  ZNET_NODISCARD bool shutdown_complete() const { return shutdown_complete_; }

  ZNET_NODISCARD int tps() const { return tps_; }

 private:
  void CheckNetwork();
  void ProcessSessions();
  void CleanupAndProcessSessions(SessionMap& map);

 private:
  std::mutex mutex_;
  ServerConfig config_;
  Ref<InetAddress> bind_address_;
  bool is_listening_ = false;
  bool is_bind_ = false;
  SocketType server_socket_ = -1;
  bool shutdown_complete_ = false;
  int tps_ = 1000;
  Scheduler scheduler_{tps_};
  Task task_;

  SessionMap sessions_;
  SessionMap pending_sessions_;
};
}  // namespace znet