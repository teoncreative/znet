//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/server.h"
#include "znet/backends/tcp.h"
#include "znet/init.h"
#include "znet/error.h"
#include "znet/server_events.h"

namespace znet {

Server::Server() : Interface() {}

Server::Server(const ServerConfig& config) : Interface(), config_(config) {
  bind_address_ = InetAddress::from(config_.bind_ip, config_.bind_port);
  backend_ = backends::CreateServerFromType(config.connection_type, bind_address_,
                                            config.child_options);
  unsigned int core_count = std::thread::hardware_concurrency();
  if (core_count == 0) {
    core_count = 1;  // unknown, and an empty pool would refuse every connection
  }
  tasks_.resize(core_count);
  for (TaskData& data : tasks_) {
    data.task_ = std::make_unique<Task>();
    data.task_->Run([this, &data]() {
      data.signal_->owner.store(std::this_thread::get_id(),
                                std::memory_order_relaxed);
      std::unique_lock<std::mutex> lock(data.signal_->mutex);
      while (!data.task_->IsStopRequested()) {
        if (data.sessions_.empty() || !backend_->IsAlive()) {
          data.signal_->cv.wait(lock, [&]() {
            return !data.sessions_.empty() || data.task_->IsStopRequested();
          });
          if (data.task_->IsStopRequested()) {
            break;
          }
        }

        // per-task scheduler: Scheduler holds tick state, so workers cannot
        // share one instance.
        data.scheduler_.Start();
        CleanupAndProcessSessions(data.sessions_, &data.session_count_);
        data.scheduler_.End();
        // sit out the rest of the tick, but return early when a backend with
        // its own receive thread reports work; otherwise an arriving datagram
        // is not looked at, let alone acked, until the next tick
        auto remaining = data.scheduler_.remaining();
        if (remaining > Scheduler::Duration::zero()) {
          data.signal_->cv.wait_for(lock, remaining, [&]() {
            return data.signal_->woken.load(std::memory_order_relaxed) ||
                   data.task_->IsStopRequested();
          });
        }
        data.signal_->woken.store(false, std::memory_order_relaxed);
      }
      for (auto&& item : data.sessions_) {
        item.second->Close();
      }
    });
  }
}

Server::~Server() {
  ZNET_LOG_DEBUG("Destructor of the server is called.");
  Stop();
  task_.Wait();
}

Result Server::Bind() {
  Result init_result = Init();
  if (ZNET_UNLIKELY(init_result != Result::Success)) ZNET_UNLIKELY_ATTR {
    ZNET_LOG_ERROR("Cannot bind because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return init_result;
  }
  Result result = backend_->Bind();
  if (result == Result::Success) {
    // the backend may have resolved an auto-assigned port
    bind_address_ = backend_->bind_address();
    // registered before Listen() starts any receive thread
    backend_->SetWakeCallback([this]() {
      for (TaskData& data : tasks_) {
        // skip workers holding no sessions: they cannot be the datagram's
        // owner, and waking them costs a thread switch each. see
        // TaskData::session_count_ for why a stale read is safe.
        if (data.session_count_.load(std::memory_order_relaxed) == 0) {
          continue;
        }
        data.signal_->Raise();
      }
    });
  }
  return result;
}

void Server::Wait() {
  task_.Wait();
}

Result Server::Listen() {
  if (task_.IsRunning()) {
    return Result::AlreadyListening;
  }
  Result result = backend_->Listen();
  if (ZNET_UNLIKELY(result != Result::Success)) ZNET_UNLIKELY_ATTR {
    return result;
  }

  shutdown_complete_ = false;

   task_.Run([this]() {
    MainProcessor();
  });
  return Result::Success;
}

Result Server::Stop() {
  return backend_->Close();
}

void Server::SetTicksPerSecond(uint16_t tps) {
  for (TaskData& data : tasks_) {
    data.scheduler_.SetTicksPerSecond(tps);
  }
}

bool Server::IsAlive() const {
  return backend_->IsAlive();
}

void Server::MainProcessor() {
  ZNET_LOG_DEBUG("Listening connections from: {}", bind_address_->readable());
  ServerStartupEvent startup_event{*this};
  event_callback()(startup_event);

  while (backend_->IsAlive() && !task_.IsStopRequested()) {
    {
      // hold the backend lock only while touching backend/session state, not
      // across the pacing wait below.
      std::lock_guard<std::mutex> lock(backend_->mutex());
      scheduler_.Start();
      CheckNetwork();
      ProcessSessions();
      scheduler_.End();
    }
    scheduler_.Wait();
  }

  ZNET_LOG_DEBUG("Shutting down server!");
  ServerShutdownEvent shutdown_event{*this};
  event_callback()(shutdown_event);

  // the receive thread's wake callback reaches into tasks_, so it has to be
  // joined before they are destroyed. the socket stays open until Close() so
  // pending sessions can still send their FINs.
  backend_->StopReceiving();
  tasks_.clear();
  DisconnectPending();
  backend_->Close();

  ZNET_LOG_DEBUG("Server shutdown complete.");
  shutdown_complete_ = true;
}

void Server::CheckNetwork() {
  // drain the accept queue each tick. For TCP this clears the listen backlog
  // faster; for ZDT, Accept() also pumps the shared UDP socket's demux, so it
  // must be called until it is drained.
  while (auto session = backend_->Accept()) {
    ZNET_LOG_DEBUG("Accepted new connection from: {}", session->remote_address()->readable());
    pending_sessions_[session->remote_address()] = session;
  }
}

void Server::CleanupAndProcessSessions(SessionMap& sessions,
                                       std::atomic<size_t>* published_count) {
  std::vector<std::shared_ptr<InetAddress>> remove;
  // cleanup dead sessions
  for (auto&& item : sessions) {
    if (item.second->IsAlive()) {
      continue;
    }
    remove.emplace_back(item.first);
  }

  for (auto&& address : remove) {
    auto session = sessions[address];
    if (session->IsReady()) {
      // this session was still pending, so no event for you!
      ServerClientDisconnectedEvent event{sessions[address]};
      event_callback()(event);

      ZNET_LOG_DEBUG("Client disconnected: {}",
                     event.session()->remote_address()->readable());
    }
    sessions.erase(address);
  }
  if (published_count) {
    published_count->store(sessions.size(), std::memory_order_relaxed);
  }

  for (auto&& item : sessions) {
    item.second->Process();
  }
}

void Server::DisconnectPending() {
  for (auto&& item : pending_sessions_) {
    item.second->Close();
  }
  ProcessSessions();
}

void Server::PromoteReady(std::shared_ptr<PeerSession> session) {
  TaskData* assign_task = SelectNextTask();
  if (assign_task && SubmitSession(*assign_task, session)) {
    return;
  }
  ZNET_LOG_DEBUG("No task is available to handle the connection from: {}", session->remote_address()->readable());
  session->Close();
}

void Server::ProcessSessions() {
  // cleanup and process pending sessions
  CleanupAndProcessSessions(pending_sessions_);

  // process pending connections and promote them
  std::vector<decltype(pending_sessions_)::key_type> promote;
  for (auto&& item : pending_sessions_) {
    if (!item.second->IsReady()) {
      if (config_.connection_timeout.count() > 0 && item.second->time_since_connect() > config_.connection_timeout) {
        ZNET_LOG_DEBUG("Pending connection from {} was timed-out.", item.second->remote_address()->readable());
        item.second->Close();
      }
      continue;
    }
    promote.emplace_back(item.first);
  }

  for (auto&& address : promote) {
    auto session = pending_sessions_[address];
    // promote to connected
    PromoteReady(session);
    // erase pending
    pending_sessions_.erase(address);
  }
}

bool Server::SubmitSession(TaskData& data, std::shared_ptr<PeerSession> session) {
  std::lock_guard<std::mutex> lock(data.signal_->mutex);
  IncomingClientConnectedEvent event{session};
  event_callback()(event);
  data.sessions_[session->remote_address()] = session;
  data.session_count_.store(data.sessions_.size(), std::memory_order_relaxed);
  // Send() only queues; without this an outbound message on an idle session
  // waits out the tick. the callback keeps the signal alive on its own, so a
  // session the application holds past shutdown still has a valid target.
  auto signal = data.signal_;
  session->SetWakeCallback([signal]() { signal->Raise(); });
  ZNET_LOG_DEBUG("New connection is ready. {}", session->remote_address()->readable());
  data.signal_->cv.notify_one();
  return true;
}

Server::TaskData* Server::SelectNextTask() {
  if (tasks_.empty()) {
    return nullptr;
  }
  // session_count_, not sessions_.size(): this runs on the acceptor while the
  // workers are mutating their own maps under their own locks. a stale count
  // only picks a slightly less idle worker.
  TaskData* min = &tasks_[0];
  size_t min_count = min->session_count_.load(std::memory_order_relaxed);
  for (TaskData& data : tasks_) {
    const size_t count = data.session_count_.load(std::memory_order_relaxed);
    if (count < min_count) {
      min = &data;
      min_count = count;
    }
  }
  return min;
}
}  // namespace znet