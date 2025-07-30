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

#include "znet/precompiled.h"
#include "znet/interface.h"
#include "znet/peer_session.h"
#include "znet/logger.h"
#include "znet/base/scheduler.h"
#include "znet/task.h"

namespace znet {

struct ServerConfig {
  std::string bind_ip;
  int bind_port;
  ConnectionType connection_type;
};

class Server : public Interface {
 public:
  using SessionMap = std::unordered_map<std::shared_ptr<InetAddress>, std::shared_ptr<PeerSession>>;

  Server();
  explicit Server(const ServerConfig& config);
  Server(const Server&) = delete;

  ~Server() override;

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
  std::shared_ptr<InetAddress> bind_address_;
  bool is_listening_ = false;
  bool is_bind_ = false;
  SocketHandle server_socket_ = -1;
  bool shutdown_complete_ = false;
  int tps_ = 1000;
  Scheduler scheduler_{tps_};
  Task task_;

  SessionMap sessions_;
  SessionMap pending_sessions_;
};
}  // namespace znet