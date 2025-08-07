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
  backend_ = backends::CreateServerFromType(config.connection_type, bind_address_);
}

Server::~Server() {
  ZNET_LOG_DEBUG("Destructor of the server is called.");
  Stop();
}

Result Server::Bind() {
  Result init_result = Init();
  if (init_result != Result::Success) {
    ZNET_LOG_ERROR("Cannot bind because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return init_result;
  }
  return backend_->Bind();
}

void Server::Wait() {
  task_.Wait();
}

Result Server::Listen() {
  if (task_.IsRunning()) {
    return Result::AlreadyListening;
  }
  Result result = backend_->Listen();
  if (result != Result::Success) {
    return result;
  }

  shutdown_complete_ = false;

  task_.Run([this]() {
    ZNET_LOG_DEBUG("Listening connections from: {}", bind_address_->readable());
    ServerStartupEvent startup_event{*this};
    event_callback()(startup_event);

    while (backend_->IsAlive()) {
      std::lock_guard<std::mutex> lock(backend_->mutex());
      scheduler_.Start();
      CheckNetwork();
      ProcessSessions();
      scheduler_.End();
      scheduler_.Wait();
    }

    ZNET_LOG_DEBUG("Shutting down server!");
    ServerShutdownEvent shutdown_event{*this};
    event_callback()(shutdown_event);

    DisconnectPeers();
    backend_->Close();

    ZNET_LOG_DEBUG("Server shutdown complete.");
    shutdown_complete_ = true;
  });
  return Result::Success;
}

Result Server::Stop() {
  return backend_->Close();
}

void Server::SetTicksPerSecond(int tps) {
  std::lock_guard<std::mutex> lock(backend_->mutex());
  tps = std::max(tps, 1);
  tps_ = tps;
  scheduler_.SetTicksPerSecond(tps);
}

void Server::CheckNetwork() {
  auto session = backend_->Accept();
  if (session != nullptr) {
    ZNET_LOG_DEBUG("Accepted new connection from: {}", session->remote_address()->readable());
    pending_sessions_[session->remote_address()] = session;
  }
}

void Server::CleanupAndProcessSessions(SessionMap& map) {
  std::vector<std::shared_ptr<InetAddress>> remove;
  // cleanup dead sessions
  for (auto&& item : map) {
    if (item.second->IsAlive()) {
      continue;
    }
    remove.emplace_back(item.first);
  }

  for (auto&& address : remove) {
    auto session = map[address];
    if (session->IsReady()) {
      // this session was still pending, so no event for you!
      ServerClientDisconnectedEvent event{map[address]};
      event_callback()(event);

      ZNET_LOG_DEBUG("Client disconnected: {}",
                     event.session()->remote_address()->readable());
    }
    map.erase(address);
  }

  for (auto&& item : map) {
    item.second->Process();
  }
}

void Server::DisconnectPeers() {
  for (auto&& item : sessions_) {
    item.second->Close();
  }
  for (auto&& item : pending_sessions_) {
    item.second->Close();
  }
  ProcessSessions();
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
    sessions_[address] = session;
    IncomingClientConnectedEvent event{session};
    event_callback()(event);
    ZNET_LOG_DEBUG("New connection is ready. {}", address->readable());
    // erase pending
    pending_sessions_.erase(address);
  }

  // process sessions
  CleanupAndProcessSessions(sessions_);
}

}  // namespace znet