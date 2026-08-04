//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_CLIENT_H_
#define ZNET_CLIENT_H_

#include "znet/options.h"
#include "znet/interface.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/scheduler.h"
#include "znet/session_encoder.h"
#include "znet/task.h"
#include "znet/worker_signal.h"

namespace znet {

namespace backends {
class ClientBackend;
}  // namespace backends

/**
 * @brief Everything a Client is constructed from.
 *
 * The aggregate is meant for brace-init:
 * `ClientConfig{"1.2.3.4", 25000, std::chrono::seconds(10)}`.
 */
struct ClientConfig {
  /** @brief Server host: an IP, a hostname, or "unix:/path" with TCP. */
  std::string server_address;
  PortNumber server_port = 0;
  /** @brief Give up on a connect that is not ready after this long. Zero
   * waits forever. */
  std::chrono::steady_clock::duration connection_timeout{
      std::chrono::seconds(10)};
  ConnectionType connection_type = ConnectionType::ZDT;
  /** @brief This client's session options; see options.h. */
  SessionOptions options;
};

/**
 * @brief Network client for managing connections and communication to a server.
 *
 * Handles client lifecycle, connecting, disconnecting, and network operations within an event-driven framework.
 */
class Client : public Interface {
 public:
  explicit Client(const ClientConfig& config);
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

  /**
   * @brief Same, but to an explicit local address: a specific interface, or a
   *        fixed source port. Same Result values as Bind().
   */
  Result Bind(const std::string& ip, PortNumber port);

  /**
   * @brief Sets how often the client's loop services the session.
   *
   * The loop sleeps out the rest of each tick, but a datagram arriving or a
   * Send() on an idle session cuts that short, so raising this trades CPU for
   * responsiveness only where neither of those applies.
   *
   * @param tps Ticks per second, clamped to at least 1.
   */
  void SetTicksPerSecond(uint16_t tps) { scheduler_.SetTicksPerSecond(tps); }

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

  /**
   * @brief This client's one session, or null before Connect(). Most code
   *        takes it from ClientConnectedToServerEvent instead, which also
   *        marks the moment it is ready.
   */
  ZNET_NODISCARD std::shared_ptr<PeerSession> client_session() const {
    return client_session_;
  }

  /**
   * @brief Drops the client's reference to a session that already ended.
   *
   * The transport closes its descriptor when the last holder lets go, and
   * that is what frees the local port: a hole punch has to bind the very
   * port the relay observed, which a merely shut-down socket still owns.
   * A session that is still alive is left alone.
   */
  void ReleaseSession();

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
  Scheduler scheduler_{120};
  std::shared_ptr<WorkerSignal> signal_{std::make_shared<WorkerSignal>()};
  // a client has one session and one loop, so without this the loop would
  // serialize encoding behind putting bytes on the wire
  SessionEncoder encoder_;
};

}  // namespace znet

#endif  // ZNET_CLIENT_H_
