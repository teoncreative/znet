//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/client.h"
#include <fcntl.h>  // For fcntl
#include "znet/event/server_events.h"
#include "znet/logger.h"

namespace znet {
Client::Client(const ClientConfig& config) : config_(config) {
  server_address_ = InetAddress::from(config_.server_ip_, config_.server_port_);
}

void Client::Bind() {
  client_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (client_socket_ < 0) {
    LOG_ERROR("Error binding socket.");
    return;
  }
}

void Client::Connect() {
  if (connect(client_socket_, server_address_->handle_ptr(),
              server_address_->addr_size()) < 0) {
    LOG_ERROR("Error connecting to server.");
    return;
  }
  int option = 1;
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option,
             sizeof(option));

  // todo local address
  client_session_ =
      CreateRef<ClientSession>(nullptr, server_address_, client_socket_);

  ClientConnectedToServerEvent event{client_session_};
  event_callback()(event);

  // Connected to the server
  LOG_INFO("Connected to the server.");

  while (client_session_->IsAlive()) {
    client_session_->Process();
  }

  close(client_socket_);
  LOG_INFO("Disconnected from the server.");
}

void Client::Disconnect() {
  client_session_->Close();
}

}  // namespace znet