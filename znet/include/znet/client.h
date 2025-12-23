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

#include "znet/backends/backend.h"
#include "znet/interface.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/task.h"

namespace znet {

struct ClientConfig {
  std::string server_ip;
  PortNumber server_port;
  std::chrono::steady_clock::duration connection_timeout;
  ConnectionType connection_type = ConnectionType::TCP;
};

/**
 * @brief Network client for managing connections and communication to a server.
 *
 * Handles client lifecycle, connecting, disconnecting, and network operations within an event-driven framework.
 */
class Client : public Interface {
 public:
  Client(const ClientConfig& config);
  Client(const Client&) = delete;
  ~Client() override;

  /**
   * @brief Binds client to configured IP address and port. This function is not thread-safe.
   *
   * @return Result::Success if binding is successful
   * @return Result::InvalidAddress if IP address is invalid
   * @return Result::CannotCreateSocket if socket creation fails
   * @return Result::CannotBind if binding to address:port fails
   * @return Result::Failure if setup process fails
   */
  Result Bind() override;

  Result Bind(const std::string& ip, PortNumber port);

  /**
   * @brief Establishes connection to the specified server address. This function is not thread-safe.
   *
   * @return Result::Success if the connection is successfully established.
   * @return Result::AlreadyConnected if a connection is already active.
   * @return Result::InvalidRemoteAddress if the server address is invalid.
   * @return Result::Failure if the connection attempt fails.
   */
  Result Connect();

  /**
   * @brief Terminates the connection. This function is not thread-safe.
   *
   * @return Result::Success if the disconnection is successful.
   * @return Result::Failure if no active session exists or disconnection fails.
   */
  Result Disconnect(CloseOptions options = {});

  /**
   * @brief Waits for the completion of the client's thread. This function is thread-safe.
   *
   * This method blocks the calling thread until the client's internal thread,
   * which handles network operations and session management, has completed (disconnected).
   *
   * Typically used during client shutdown or when there is a need to
   * synchronize the caller with the client's task execution.
   *
   */
  void Wait() override;

  ZNET_NODISCARD std::shared_ptr<PeerSession> client_session() const {
    return client_session_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> server_address() const {
    return server_address_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const;

 private:
  ClientConfig config_;
  std::shared_ptr<InetAddress> server_address_;
  std::unique_ptr<backends::ClientBackend> backend_;
  std::shared_ptr<PeerSession> client_session_;

  Task task_;

};

}  // namespace znet