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
  /**
   * @brief Installs a callback fired when inbound data arrives.
   *
   * Mirrors the server-side hook: called from whichever thread noticed the
   * data, so the client's loop does not sleep out its tick with work waiting.
   * Backends that read on the caller's thread ignore it.
   */
  virtual void SetWakeCallback(std::function<void()> on_data) { (void)on_data; }

  /** @brief Joins any receive thread, leaving the socket open. Idempotent. */
  virtual void StopReceiving() {}

  /**
   * @brief Whether the backend has its own thread taking datagrams off the
   *        socket.
   *
   * When it does, the client's loop can sleep out its tick and be woken on
   * arrival. When it does not, reads only happen while the loop is running, so
   * pacing it would add a tick of latency to everything inbound.
   */
  virtual bool DrivesOwnReceive() const { return false; }
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

  /**
   * @brief The address actually bound.
   *
   * Differs from the one requested when the port was auto-assigned.
   */
  virtual std::shared_ptr<InetAddress> bind_address() const = 0;

  /** @brief Backend-level counters. Backends that track none return zeroes. */
  virtual ServerMetrics metrics() const { return {}; }

  /**
   * @brief Installs a callback fired when inbound data arrives.
   *
   * Called from whichever thread noticed the data, to cut short a session
   * worker that would otherwise sleep out the rest of its tick. Must be cheap
   * and must not call back into the backend. Backends that read on the
   * caller's thread ignore it.
   *
   * @param on_data Invoked on arrival; pass nothing to leave the default no-op.
   */
  virtual void SetWakeCallback(std::function<void()> on_data) {
    (void)on_data;
  }

  /**
   * @brief Joins any receive thread the backend runs, leaving the socket open.
   *
   * Call before destroying anything the wake callback touches. The socket
   * stays usable so sessions can still send their goodbyes; Close() releases
   * it. Idempotent, and a no-op for backends without their own thread.
   */
  virtual void StopReceiving() {}
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
