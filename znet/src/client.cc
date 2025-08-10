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
#include "znet/backends/tcp.h"
#include "znet/client_events.h"
#include "znet/error.h"
#include "znet/init.h"
#include "znet/logger.h"

namespace znet {
Client::Client(const ClientConfig& config) : config_(config) {
  server_address_ = InetAddress::from(config_.server_ip, config_.server_port);
  backend_ = backends::CreateClientFromType(config.connection_type, server_address_);
}

Client::~Client() {
  ZNET_LOG_DEBUG("Destructor of the client is called.");
  Disconnect();
}

Result Client::Bind() {
  Result init_result = Init();
  if (init_result != Result::Success) {
    ZNET_LOG_ERROR("Cannot bind because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return init_result;
  }
  if (!backend_) {
    return Result::InvalidBackend;
  }
  return backend_->Bind();
}

Result Client::Bind(const std::string& ip, PortNumber port) {
  Result init_result = Init();
  if (init_result != Result::Success) {
    ZNET_LOG_ERROR("Cannot bind because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return init_result;
  }
  if (!backend_) {
    return Result::InvalidBackend;
  }
  return backend_->Bind(ip, port);
}

Result Client::Connect() {
  if (task_.IsRunning()) {
    return Result::AlreadyConnected;
  }
  if (!backend_) {
    return Result::InvalidBackend;
  }
  Result result = backend_->Connect();
  if (result != Result::Success) {
    return result;
  }

  client_session_ = backend_->client_session();

  // Connected to the server
  task_.Run([this]() {
    // setup
    while (!client_session_->IsReady() && client_session_->IsAlive()) {
      client_session_->Process();
      if (config_.connection_timeout.count() > 0 && client_session_->time_since_connect() > config_.connection_timeout) {
        ZNET_LOG_DEBUG("Connection to {} timed-out.", server_address_->readable());
        client_session_->Close();
      }
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

ZNET_NODISCARD std::shared_ptr<InetAddress> Client::local_address() const {
  return backend_->local_address();
}

}  // namespace znet