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
#include <fcntl.h>  // For fcntl
#include "znet/event/server_events.h"

namespace znet {

Server::Server() : Interface() {}

Server::Server(const ServerConfig& config) : Interface(), config_(config) {}

Server::~Server() {
#ifdef TARGET_WIN
  WSACleanup();
#endif
}

void Server::Bind() {
  bind_address_ = InetAddress::from(config_.bind_ip_, config_.bind_port_);

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
    exit(-1);
  }
#endif
  server_socket_ = socket(
      domain, SOCK_STREAM,
      0);  // SOCK_STREAM for TCP, SOCK_DGRAM for UDP, there is also SOCK_RAW,
           // but we don't care about that.
#ifdef TARGET_WIN
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, &option,
             sizeof(option));
#else
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option,
             sizeof(option));
#endif
  if (server_socket_ == -1) {
#ifdef TARGET_WIN
    ZNET_LOG_ERROR("Error creating socket. {}", WSAGetLastError());
#else
    ZNET_LOG_ERROR("Error creating socket.");
#endif
    exit(-1);
  }
#ifdef TARGET_WIN
  u_long mode = 1;  // 1 to enable non-blocking socket
  ioctlsocket(server_socket_, FIONBIO, &mode);
#else
  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_INFO("Error getting socket flags.");
    close(server_socket_);
    return;
  }
  if (fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_INFO("Error setting socket to non-blocking mode.");
    close(server_socket_);
    return;
  }
#endif

  bind(server_socket_, bind_address_->handle_ptr(), bind_address_->addr_size());
  ZNET_LOG_INFO("Listening connections from: {}", bind_address_->readable());
}

void Server::Listen() {
  listen(server_socket_, SOMAXCONN);

  is_listening_ = true;
  shutdown_complete_ = false;

  while (is_listening_) {
    CheckNetwork();
    ProcessSessions();
  }

  ZNET_LOG_INFO("Shutting down server!");
  // Disconnect all sessions
  for (const auto& item : sessions_) {
    item.second->Close();
  }
  sessions_.clear();
  // Close the server
#ifdef TARGET_WIN
  closesocket(server_socket_);
#else
  close(server_socket_);
#endif
  ZNET_LOG_INFO("Server shutdown complete.");
  shutdown_complete_ = true;
}

void Server::Stop() {
  is_listening_ = false;
}

void Server::CheckNetwork() {
  sockaddr client_address{};
  socklen_t addr_len = sizeof(client_address);
  SocketType client_socket = accept(server_socket_, &client_address, &addr_len);
  if (client_socket < 0) {
    return;
  }
#ifdef TARGET_WIN
  u_long mode = 1;  // 1 to enable non-blocking socket
  ioctlsocket(server_socket_, FIONBIO, &mode);
#else
  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_INFO("Error getting socket flags.");
    close(client_socket);
    return;
  }
  if (fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_INFO("Error setting socket to non-blocking mode.");
    close(client_socket);
    return;
  }
#endif
  Ref<InetAddress> remote_address = InetAddress::from(&client_address);
  if (remote_address == nullptr) {
    return;
  }
  auto session =
      CreateRef<ServerSession>(bind_address_, remote_address, client_socket);
  sessions_[remote_address] = session;
  ServerClientConnectedEvent event{session};
  event_callback()(event);
  ZNET_LOG_INFO("New connection is ready. {}", remote_address->readable());
}

void Server::ProcessSessions() {
  std::vector<decltype(sessions_)::key_type> vec;
  for (auto&& item : sessions_) {
    if (!item.second->IsAlive()) {
      vec.emplace_back(item.first);
      continue;
    }
    item.second->Process();
  }

  for (auto&& key : vec) {
    ServerClientDisconnectedEvent event{sessions_[key]};
    event_callback()(event);

    ZNET_LOG_INFO("Client disconnected.");
    sessions_.erase(key);
  }
}

}  // namespace znet