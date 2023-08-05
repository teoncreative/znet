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

Client::~Client() {
#ifdef TARGET_WIN
  WSACleanup();
#endif
}

Result Client::Bind() {
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
  client_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (client_socket_ < 0) {
    ZNET_LOG_ERROR("Error binding socket.");
    return Result::CannotBind;
  }
}

Result Client::Connect() {
  if (!server_address_) {
    return Result::InvalidAddress;
  }
  if (connect(client_socket_, server_address_->handle_ptr(),
                                  server_address_->addr_size()) < 0) {
    ZNET_LOG_ERROR("Error connecting to server.");
    return Result::Failure;
  }
  const char option = 1;
#ifdef TARGET_WIN
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, &option,
             sizeof(option));
#else
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option,
             sizeof(option));
#endif
  // todo local address
  client_session_ =
      CreateRef<ClientSession>(nullptr, server_address_, client_socket_);

  ClientConnectedToServerEvent event{client_session_};
  event_callback()(event);

  // Connected to the server
  ZNET_LOG_DEBUG("Connected to the server.");

  while (client_session_->IsAlive()) {
    client_session_->Process();
  }

#ifdef TARGET_WIN
  closesocket(client_socket_);
#else
  close(client_socket_);
#endif
  ZNET_LOG_DEBUG("Disconnected from the server.");
  return Result::Completed;
}

Result Client::Disconnect() {
  return client_session_->Close();
}

}  // namespace znet