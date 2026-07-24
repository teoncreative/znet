//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 06/08/2025.
//

#ifndef ZNET_PARENT_BACKEND_H
#define ZNET_PARENT_BACKEND_H

#include "znet/metrics.h"
#include "znet/options.h"
#include "znet/peer_session.h"

namespace znet {
namespace backends {

class ClientBackend {
 public:
  virtual ~ClientBackend() = default;

  virtual Result Bind() = 0;
  virtual Result Bind(const std::string& ip, PortNumber port) = 0;
  virtual Result Connect() = 0;
  virtual Result Close() = 0;
  virtual void Update() = 0;

  virtual bool IsAlive() = 0;

  virtual std::shared_ptr<PeerSession> client_session() = 0;
  virtual std::shared_ptr<InetAddress> local_address() = 0;

  virtual std::mutex& mutex() = 0;
};

class ServerBackend {
 public:
  virtual ~ServerBackend() = default;

  virtual Result Bind() = 0;
  virtual Result Listen() = 0;
  virtual Result Close() = 0;

  virtual void Update() = 0;

  virtual std::shared_ptr<PeerSession> Accept() = 0;
  virtual void AcceptAndReject() = 0;

  virtual bool IsAlive() = 0;

  virtual std::mutex& mutex() = 0;

  // The address actually bound, which may differ from the one requested when the
  // port was auto-assigned.
  virtual std::shared_ptr<InetAddress> bind_address() const = 0;

  // Backend-level counters; backends that track none return a zeroed struct.
  virtual ServerMetrics metrics() const { return {}; }
};

std::unique_ptr<ClientBackend> CreateClientFromType(
    ConnectionType type, std::shared_ptr<InetAddress> server_address,
    const SessionOptions& options = {});

std::unique_ptr<ServerBackend> CreateServerFromType(
    ConnectionType type, std::shared_ptr<InetAddress> bind_address,
    const SessionOptions& child_options = {});

}
}

#endif  //ZNET_PARENT_BACKEND_H
