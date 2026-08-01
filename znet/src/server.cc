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
  tasks_.reserve(core_count);
  for (uint32_t i = 0; i < core_count; i++) {
    tasks_.push_back(std::make_unique<TaskData>());
    TaskData& data = *tasks_.back();
    data.task_ = std::make_unique<Task>();
    data.task_->Run([this, &data]() { WorkerLoop(data); });
  }
}

void Server::WorkerLoop(TaskData& data) {
  WorkerSignal& signal = *data.signal_;
  signal.owner.store(std::this_thread::get_id(), std::memory_order_relaxed);

  while (!data.task_->IsStopRequested()) {
    // nothing to drive yet: sleep until a session is handed over, with no
    // deadline, since no tick is owed on an empty worker
    if (data.sessions_.count() == 0 || !backend_->IsAlive()) {
      std::unique_lock<std::mutex> lock(signal.mutex);
      signal.cv.wait(lock, [&]() {
        return data.sessions_.count() != 0 || data.task_->IsStopRequested();
      });
      if (data.task_->IsStopRequested()) {
        break;
      }
    }

    // per-task scheduler: Scheduler holds tick state, so workers cannot share
    // one instance.
    data.scheduler_.Start();
    data.sessions_.With(
        [this](SessionMap& sessions) { CleanupAndProcessSessions(sessions); });
    data.scheduler_.End();

    // sit out the rest of the tick, but return early when a backend with its
    // own receive thread reports work; otherwise an arriving datagram is not
    // looked at, let alone acked, until the next tick
    const auto remaining = data.scheduler_.remaining();
    if (remaining > Scheduler::Duration::zero()) {
      std::unique_lock<std::mutex> lock(signal.mutex);
      signal.cv.wait_for(lock, remaining, [&]() {
        return signal.woken.load(std::memory_order_relaxed) ||
               data.task_->IsStopRequested();
      });
      signal.woken.store(false, std::memory_order_relaxed);
    }
  }

  data.sessions_.With([](SessionMap& sessions) {
    for (auto&& item : sessions) {
      item.second->Close();
      // still on the worker, and it is about to exit, so this is the last
      // chance to break a handler->session cycle before ~TaskData drops the
      // map. closing alone would not: a cycle keeps both ends alive whether
      // the transport is open or not.
      item.second->ReleaseHandler();
    }
  });
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
      for (auto& data : tasks_) {
        // skip workers holding no sessions: they cannot be the datagram's
        // owner, and waking them costs a thread switch each. see SessionSet
        // for why a stale count is safe here.
        if (data->sessions_.count() == 0) {
          continue;
        }
        data->signal_->Raise();
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
  for (auto& data : tasks_) {
    data->scheduler_.SetTicksPerSecond(tps);
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
    scheduler_.Start();
    CheckNetwork();
    ProcessSessions();
    scheduler_.End();
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

void Server::CleanupAndProcessSessions(SessionMap& sessions) {
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
    // one that never became ready died still handshaking, and the application
    // was never told it connected, so a disconnect event would be unpaired.
    if (session->IsReady()) {
      ServerClientDisconnectedEvent event{session};
      event_callback()(event);
      ZNET_LOG_DEBUG("Client disconnected: {}",
                     session->remote_address()->readable());
    }
    // the map is about to drop its reference, and a handler holding one back
    // to the session would be the only thing left pointing at either of them.
    // see PeerSession::ReleaseHandler. this runs on the worker, the same
    // thread that dispatches into the handler, so it cannot race one.
    session->ReleaseHandler();
    sessions.erase(address);
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
  TaskData* task = SelectNextTask();
  if (!task) {
    ZNET_LOG_DEBUG("No worker is available to handle the connection from: {}",
                   session->remote_address()->readable());
    session->Close();
    return;
  }
  SubmitSession(*task, session);
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

void Server::SubmitSession(TaskData& data, std::shared_ptr<PeerSession> session) {
  auto signal = data.signal_;
  session->SetWakeCallback([signal]() { signal->Raise(); });
  IncomingClientConnectedEvent event{session};
  event_callback()(event);
  data.sessions_.With([&](SessionMap& sessions) {
    sessions[session->remote_address()] = session;
  });
  ZNET_LOG_DEBUG("New connection is ready. {}", session->remote_address()->readable());
  data.signal_->Raise();
}

Server::TaskData* Server::SelectNextTask() {
  if (tasks_.empty()) {
    return nullptr;
  }
  // the published count, not the map: this runs on the acceptor while the
  // workers are mutating their own maps under their own locks. a stale count
  // only picks a slightly less idle worker.
  TaskData* min = tasks_[0].get();
  size_t min_count = min->sessions_.count();
  for (auto& data : tasks_) {
    const size_t count = data->sessions_.count();
    if (count < min_count) {
      min = data.get();
      min_count = count;
    }
  }
  return min;
}
}  // namespace znet