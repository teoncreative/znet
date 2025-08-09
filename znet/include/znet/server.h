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

#include "znet/base/scheduler.h"
#include "znet/interface.h"
#include "znet/logger.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/task.h"
#include "znet/backends/backend.h"

namespace znet {

struct ServerConfig {
  std::string bind_ip;
  PortNumber bind_port;
  std::chrono::steady_clock::duration connection_timeout;
  ConnectionType connection_type = ConnectionType::TCP;
};

/**
 * @brief Network server managing connections and peer sessions.
 *
 * Handles server lifecycle, connection management, and network operations within an event-driven framework.
 */
class Server : public Interface {
 public:
  using SessionMap = std::unordered_map<std::shared_ptr<InetAddress>,
                                        std::shared_ptr<PeerSession>>;

  Server();
  explicit Server(const ServerConfig& config);
  Server(const Server&) = delete;

  ~Server() override;

  /**
   * @brief Binds server to configured IP address and port. This function is not thread-safe.
   *
   * @return Result::Success if binding is successful
   * @return Result::InvalidAddress if IP address is invalid
   * @return Result::CannotCreateSocket if socket creation fails
   * @return Result::CannotBind if binding to address:port fails
   * @return Result::Failure if setup process fails
   */
  Result Bind() override;

  /**
   * @brief Starts server and accepts incoming connections. This function is not thread-safe.
   *
   * Initializes server's listening state and begins accepting client connections.
   * Creates a task to manage network operations and session handling.
   *
   * This function does not block the main thread, it does its listening in its
   * own thread. (async)
   *
   * @return Result::Success if server starts successfully
   * @return Result::AlreadyListening if already in listening state
   * @return Result::CannotListen if listening fails
   */
  Result Listen();

  /**
   * @brief Waits for the completion of the server's thread. This function is thread-safe.
   *
   * This method blocks the calling thread until the server's internal thread,
   * which handles network operations and session management, has completed.
   *
   * Typically used during server shutdown or when there is a need to
   * synchronize the caller with the server's task execution.
   *
   */
  void Wait() override;

  /**
   * @brief Stops the server and releases associated resources. This function is thread-safe.
   *
   * This method stops the server from listening for incoming connections
   * and terminates its active listening state.
   *
   * @return Result::Success if the server was successfully stopped.
   * @return Result::AlreadyStopped if the server was already stopped and
   *         not in a listening state.
   */
  Result Stop();

  /**
   * @brief Configures the number of ticks per second for the server. This function is thread-safe.
   *
   * Adjusts the server's tick rate to control the frequency of internal updates.
   * Ensures the tick rate is set to a minimum of 1 tick per second, preventing invalid values.
   *
   * @param tps The desired ticks per second, must be greater than or equal to 1.
   */
  void SetTicksPerSecond(int tps);

  ZNET_NODISCARD bool shutdown_complete() const { return shutdown_complete_; }

 std::shared_ptr<InetAddress> bind_address() const { return bind_address_; }

  bool IsAlive() const;

 private:
  struct TaskData {
    std::mutex mutex_;
    SessionMap sessions_;
    std::unique_ptr<Task> task_;
    bool running_ = true;
    Scheduler scheduler_{120};
    std::condition_variable cv_;

    TaskData() = default;
    ~TaskData() {
      running_ = false;
      cv_.notify_all();
      if (task_) {
        task_->Wait();
      }
      sessions_.clear();
    }
    TaskData(TaskData&& other) noexcept
        : sessions_(std::move(other.sessions_)),
          task_(std::move(other.task_)),
          running_(other.running_) {
      // new mutex_ is default constructed
    }

    TaskData& operator=(TaskData&& other) noexcept {
      if (this != &other) {
        std::lock_guard<std::mutex> lock(other.mutex_);
        sessions_ = std::move(other.sessions_);
        task_ = std::move(other.task_);
        running_ = other.running_;
        // mutex_ is not moved — stays as-is
      }
      return *this;
    }
    TaskData(const TaskData&) = delete;
    TaskData& operator=(const TaskData&) = delete;
  };

  void MainProcessor();

  void CheckNetwork();
  void ProcessSessions();
  void CleanupAndProcessSessions(SessionMap& sessions);
  void DisconnectPending();
  void PromoteReady(std::shared_ptr<PeerSession> session);
  bool SubmitSession(TaskData& data, std::shared_ptr<PeerSession> session);
  TaskData* SelectNextTask();

 private:
  std::shared_ptr<InetAddress> bind_address_;
  std::unique_ptr<backends::ServerBackend> backend_;

  ServerConfig config_;
  bool shutdown_complete_ = false;
  Scheduler scheduler_{60};
  Task task_;

  std::vector<TaskData> tasks_;
  SessionMap pending_sessions_;
};
}  // namespace znet