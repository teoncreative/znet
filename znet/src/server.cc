//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "server.h"
#include "base/scheduler.h"
#include "error.h"
#include "server_events.h"

namespace znet {

Server::Server() : Interface() {}

Server::Server(const ServerConfig& config) : Interface(), config_(config) {}

Server::~Server() {
#ifdef TARGET_WIN
  WSACleanup();
#endif
}

Result Server::Bind() {
  bind_address_ = InetAddress::from(config_.bind_ip, config_.bind_port);
  if (!bind_address_ || !bind_address_->is_valid()) {
    return Result::InvalidAddress;
  }

  const char option = 1;

  int domain = GetDomainByInetProtocolVersion(bind_address_->ipv());
#ifdef TARGET_WIN
  WORD wVersionRequested;
  WSADATA wsaData;
  int err;
  /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
  wVersionRequested = MAKEWORD(2, 2);
  err = WSAStartup(wVersionRequested, &wsaData);
  if (err != 0) {
    ZNET_LOG_ERROR("WSAStartup error. {}", err);
    return Result::Failure;
  }
#endif
  server_socket_ = socket(
      domain, SOCK_STREAM,
      0);  // SOCK_STREAM for TCP, SOCK_DGRAM for UDP, there is also SOCK_RAW,
           // but we don't care about that.
  if (server_socket_ == -1) {
    ZNET_LOG_ERROR("Error creating socket. {}", GetLastErrorInfo());
    return Result::CannotCreateSocket;
  }
#ifdef TARGET_WIN
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, &option,
             sizeof(option));
#else
  setsockopt(server_socket_, SOL_SOCKET,
             SO_REUSEADDR | SO_REUSEPORT | SO_BROADCAST, &option,
             sizeof(option));
#endif
#ifdef TARGET_WIN
  u_long mode = 1;  // 1 to enable non-blocking socket
  ioctlsocket(server_socket_, FIONBIO, &mode);
#else
  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_ERROR("Error getting socket flags: {}", GetLastErrorInfo());
    close(server_socket_);
    return Result::Failure;
  }
  if (fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    close(server_socket_);
    return Result::Failure;
  }
#endif
  if (bind(server_socket_, bind_address_->handle_ptr(),
           bind_address_->addr_size()) != 0) {
    ZNET_LOG_DEBUG("Failed to bind: {}, {}", bind_address_->readable(),
                   GetLastErrorInfo());
    return Result::CannotBind;
  }
  is_bind_ = true;
  ZNET_LOG_DEBUG("Bind to: {}", bind_address_->readable());
  return Result::Success;
}

void Server::Wait() {
  task_.Wait();
}

Result Server::Listen() {
  if (task_.IsRunning()) {
    return Result::AlreadyListening;
  }
  if (!is_bind_) {
    return Result::CannotListen;
  }
  if (listen(server_socket_, SOMAXCONN) != 0) {
    ZNET_LOG_DEBUG("Failed to listen connections from: {}, {}",
                   bind_address_->readable(), GetLastErrorInfo());
    return Result::CannotListen;
  }

  is_listening_ = true;
  shutdown_complete_ = false;

  task_.Run([this]() {
    ZNET_LOG_DEBUG("Listening connections from: {}", bind_address_->readable());
    ServerStartupEvent startup_event{*this};
    event_callback()(startup_event);

    while (is_listening_) {
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler_.Start();
      CheckNetwork();
      ProcessSessions();
      scheduler_.End();
      scheduler_.Wait();
    }

    ZNET_LOG_DEBUG("Shutting down server!");
    ServerShutdownEvent shutdown_event{*this};
    event_callback()(shutdown_event);

    // Disconnect all sessions
    for (const auto& item : sessions_) {
      item.second->Close();
    }
    sessions_.clear();

    // Close the server
#ifdef TARGET_WIN
    if (closesocket(server_socket_) != 0) {
      ZNET_LOG_DEBUG("Failed to close socket: {}, {}",
                     bind_address_->readable(), GetLastErrorInfo());
    }
#else
    if (close(server_socket_) != 0) {
      ZNET_LOG_DEBUG("Failed to close socket: {}, {}",
                     bind_address_->readable(), GetLastErrorInfo());
    }
#endif

    ZNET_LOG_DEBUG("Server shutdown complete.");
    shutdown_complete_ = true;
  });
  return Result::Success;
}

Result Server::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_listening_) {
    return Result::AlreadyStopped;
  }
  is_listening_ = false;
  is_bind_ = false;
  return Result::Success;
}

void Server::SetTicksPerSecond(int tps) {
  std::lock_guard<std::mutex> lock(mutex_);
  tps = std::max(tps, 1);
  tps_ = tps;
  scheduler_.SetTicksPerSecond(tps);
}

void Server::CheckNetwork() {
  sockaddr client_address{};
  socklen_t addr_len = sizeof(client_address);
  SocketHandle client_socket = accept(server_socket_, &client_address, &addr_len);
#ifdef TARGET_WIN
  if (client_socket == INVALID_SOCKET) {
    return;
  }
  u_long mode = 1;  // 1 to enable non-blocking socket
  ioctlsocket(server_socket_, FIONBIO, &mode);
#else
  if (client_socket <= 0) {
    return;
  }
  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_ERROR("Error getting socket flags: {}", GetLastErrorInfo());
    close(client_socket);
    return;
  }
  if (fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    close(client_socket);
    return;
  }
#endif
  Ref<InetAddress> remote_address = InetAddress::from(&client_address);
  if (remote_address == nullptr) {
    return;
  }
  auto session =
      CreateRef<PeerSession>(bind_address_, remote_address, client_socket);
  pending_sessions_[remote_address] = session;
}

void Server::CleanupAndProcessSessions(SessionMap& map) {
  std::vector<Ref<InetAddress>> remove;
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

void Server::ProcessSessions() {
  // cleanup and process pending sessions
  CleanupAndProcessSessions(pending_sessions_);

  // process pending connections and promote them
  std::vector<decltype(pending_sessions_)::key_type> promote;
  for (auto&& item : pending_sessions_) {
    if (!item.second->IsReady()) {
      continue;
    }
    promote.emplace_back(item.first);
  }

  for (auto&& address : promote) {
    auto session = pending_sessions_[address];
    // promote to connected
    sessions_[address] = session;
    ServerClientConnectedEvent event{session};
    event_callback()(event);
    ZNET_LOG_DEBUG("New connection is ready. {}", address->readable());
    // erase pending
    pending_sessions_.erase(address);
  }

  // process sessions
  CleanupAndProcessSessions(sessions_);
}

}  // namespace znet