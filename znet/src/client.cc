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

#include "znet/client_events.h"
#include "znet/error.h"
#include "znet/logger.h"

namespace znet {
Client::Client(const ClientConfig& config) : config_(config) {
  server_address_ = InetAddress::from(config_.server_ip, config_.server_port);
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
  return Result::Success;
}

Result Client::Connect() {
  if (task_.IsRunning()) {
    return Result::AlreadyConnected;
  }
  if (!server_address_ || !server_address_->is_valid()) {
    return Result::InvalidRemoteAddress;
  }
  if (connect(client_socket_, server_address_->handle_ptr(),
              server_address_->addr_size()) < 0) {
    ZNET_LOG_ERROR("Error connecting to server: {}", GetLastErrorInfo());
    return Result::Failure;
  }
  const char option = 1;
#ifdef TARGET_WIN
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, &option,
             sizeof(option));
#else
  setsockopt(client_socket_, SOL_SOCKET,
             SO_REUSEADDR | SO_REUSEPORT | SO_BROADCAST, &option,
             sizeof(option));
#endif
  std::shared_ptr<InetAddress> local_address;

  sockaddr local_ss{};
  socklen_t local_len = sizeof(local_ss);
  if (getsockname(client_socket_, &local_ss, &local_len) == 0) {
    local_address = InetAddress::from(&local_ss);
  } else {
    ZNET_LOG_ERROR("getsockname failed, local address will be nullptr: {}", GetLastErrorInfo());
  }

  client_session_ =
      std::make_shared<PeerSession>(local_address, server_address_, client_socket_, true);

  // Connected to the server
  task_.Run([this]() {
    // setup
    while (!client_session_->IsReady() && client_session_->IsAlive()) {
      client_session_->Process();
    }
    if (!client_session_->IsAlive()) {
      return;
    }
    ZNET_LOG_DEBUG("Connected to the server.");
    ClientConnectedToServerEvent connected_event{client_session_};
    event_callback()(connected_event);
    while (client_session_->IsAlive()) {
      client_session_->Process();
    }
    ZNET_LOG_DEBUG("Disconnected from the server.");
    ClientDisconnectedFromServerEvent disconnected_event{client_session_};
    event_callback()(disconnected_event);
  });
  return Result::Success;
}

void Client::Wait() {
  task_.Wait();
}

Result Client::Disconnect() {
  if (!client_session_) {
    return Result::Failure;
  }
  return client_session_->Close();
}

}  // namespace znet