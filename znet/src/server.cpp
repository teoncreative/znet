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
void Server::Bind() {
  bind_address_ = InetAddress::from(config_.bind_ip_, config_.bind_port_);

  int option = 1;
  int domain;
  switch (bind_address_->ipv()) {
    case InetProtocolVersion::IPv4:
      domain = AF_INET;
      break;
    case InetProtocolVersion::IPv6:
      domain = AF_INET6;
      break;
  }
  server_socket_ = socket(
      domain, SOCK_STREAM,
      0);  // SOCK_STREAM for TCP, SOCK_DGRAM for UDP, there is also SOCK_RAW, but we don't care about that.
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option,
             sizeof(option));
  if (server_socket_ == -1) {
    // todo error handling
    exit(-1);
  }
  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_INFO("Error getting socket flags.");
    return;
  }
  if (fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_INFO("Error setting socket to non-blocking mode.");
    return;
  }

  bind(server_socket_, bind_address_->handle_ptr(), bind_address_->addr_size());
  ZNET_LOG_INFO("Listening connections from: {}", bind_address_->readable());
}

void Server::Listen() {
  listen(server_socket_, SOMAXCONN);

  is_listening_ = true;

  while (is_listening_) {
    CheckNetwork();
    ProcessSessions();
  }
}

void Server::Stop() {
  is_listening_ = false;
}

void Server::CheckNetwork() {
  sockaddr client_address{};
  socklen_t addr_len = sizeof(client_address);
  int client_socket = accept(server_socket_, &client_address, &addr_len);
  if (client_socket < 0) {
    return;
  }
  ZNET_LOG_INFO("Accepted new connection.");
  // Set socket to non-blocking mode
  int flags = fcntl(client_socket, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_INFO("Error getting socket flags.");
    close(client_socket);
    return;
  }
  if (fcntl(client_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_INFO("Error setting socket to non-blocking mode.");
    close(client_socket);
    return;
  }
  Ref<InetAddress> remote_address = InetAddress::from(&client_address);
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
    ZNET_LOG_INFO("Client disconnected.");
    sessions_.erase(key);
  }
}

}  // namespace znet