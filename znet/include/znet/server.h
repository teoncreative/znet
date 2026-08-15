//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_SERVER_H_
#define ZNET_SERVER_H_

#include "znet/compat.h"
#include "znet/interface.h"
#include "znet/logger.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/scheduler.h"
#include "znet/task.h"
#include "znet/worker_signal.h"

namespace znet {

namespace backends {
class ServerBackend;
}  // namespace backends

/**
 * @brief Everything a Server is constructed from.
 *
 * The aggregate is meant for brace-init:
 * `ServerConfig{"0.0.0.0", 25000, std::chrono::seconds(10)}`.
 */
struct ServerConfig {
  /** @brief Address to listen on: an IP, or "unix:/path" with TCP. */
  std::string bind_address;
  /** @brief Zero lets the system pick; read it back with bind_address(). */
  PortNumber bind_port = 0;
  /** @brief Drop an accepted connection whose handshake has not settled
   * after this long. Zero waits forever. */
  std::chrono::steady_clock::duration connection_timeout{
      std::chrono::seconds(10)};
  ConnectionType connection_type = ConnectionType::ZDT;
  ServerOptions options;         // the listener itself
  SessionOptions child_options;  // every session the listener accepts
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
  void SetTicksPerSecond(uint16_t tps);

  /**
   * @brief Whether Stop() (or a fatal error) has fully torn the server down.
   *
   * The shutdown-wait idiom: spin or sleep on this after Stop(). IsAlive() is
   * the complementary liveness check while the server runs; the two disagree
   * only during the teardown window.
   */
  ZNET_NODISCARD bool shutdown_complete() const { return shutdown_complete_; }

  /**
   * @brief The address actually bound, which differs from the configured one
   *        when bind_port was zero. Null before Bind().
   */
  ZNET_NODISCARD std::shared_ptr<InetAddress> bind_address() const {
    return bind_address_;
  }

  ZNET_NODISCARD bool IsAlive() const;

  /**
   * @brief Returns a snapshot of the server's counters.
   *
   * Backend-level totals (accepted, rejected, rate-limited, ...). Per-connection
   * numbers live on PeerSession::metrics(). Zeroed when built with
   * ZNET_ENABLE_METRICS=0, or on a backend that tracks none.
   */
  ZNET_NODISCARD ServerMetrics metrics() const;

 private:
  /**
   * @brief One worker's sessions, plus the size it publishes for the acceptor.
   *
   * The published count is what lets the receive thread's wake callback skip
   * idle workers, and SelectNextTask() pick the emptiest, without taking the
   * lock. A stale read costs at most a spurious wake or a slightly worse
   * placement, where locking per datagram would put the receive thread behind
   * whichever worker is mid-tick. Every mutation goes through With(), which
   * republishes it, so the two cannot drift.
   */
  class SessionSet {
   public:
    template <typename Fn>
    void With(Fn&& fn) {
      std::lock_guard<std::mutex> lock(mutex_);
      fn(sessions_);
      count_.store(sessions_.size(), std::memory_order_relaxed);
    }

    ZNET_NODISCARD size_t count() const {
      return count_.load(std::memory_order_relaxed);
    }

   private:
    std::mutex mutex_;
    SessionMap sessions_;
    std::atomic<size_t> count_{0};
  };

  // Not movable or copyable: it owns a thread, a mutex and a condition
  // variable. Held by unique_ptr in tasks_ so the vector never needs to be.
  struct TaskData {
    std::shared_ptr<WorkerSignal> signal_{std::make_shared<WorkerSignal>()};
    SessionSet sessions_;
    std::unique_ptr<Task> task_;
    Scheduler scheduler_{120};

    TaskData() = default;
    ~TaskData() {
      if (task_) {
        // the stop flag is part of the wait predicate, so it has to be set with
        // the mutex held. signaling outside it lets a worker that is between
        // testing the predicate and sleeping miss the wake and never return.
        {
          std::lock_guard<std::mutex> lock(signal_->mutex);
          task_->RequestStop();
        }
        signal_->cv.notify_all();
        task_->Wait();
      }
    }

    TaskData(const TaskData&) = delete;
    TaskData& operator=(const TaskData&) = delete;
  };

  void MainProcessor();
  /** @brief One worker's whole life: tick its sessions, sleep out the rest. */
  void WorkerLoop(TaskData& data);

  void CheckNetwork();
  void ProcessSessions();
  /** @brief Drops dead sessions from `sessions`, then ticks the survivors. */
  void CleanupAndProcessSessions(SessionMap& sessions);
  void DisconnectPending();
  void PromoteReady(std::shared_ptr<PeerSession> session);
  void SubmitSession(TaskData& data, std::shared_ptr<PeerSession> session);
  TaskData* SelectNextTask();
  /** @brief Handshaking plus promoted sessions. Worker counts may lag a
      tick, which only makes the max_connections check slightly lenient. */
  size_t ActiveSessionCount() const;

 private:
  std::shared_ptr<InetAddress> bind_address_;
  std::unique_ptr<backends::ServerBackend> backend_;

  ServerConfig config_;
  bool shutdown_complete_ = false;
  Scheduler scheduler_{60};
  Task task_;

  std::vector<std::unique_ptr<TaskData>> tasks_;
  SessionMap pending_sessions_;
};
}  // namespace znet

#endif  // ZNET_SERVER_H_
