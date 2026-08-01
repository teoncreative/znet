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
  backend_ = backends::CreateClientFromType(config.connection_type, server_address_,
                                            config.options);
}

Client::~Client() {
  ZNET_LOG_DEBUG("Destructor of the client is called.");
  Disconnect();
  // same reason as ~PeerSession: the loop reads signal_, scheduler_ and the
  // session, all declared after task_ and so destroyed before ~Task would get
  // round to joining it.
  task_.RequestStop();
  task_.Wait();
  // after the join, not before: releasing while the loop was still dispatching
  // would race it. a handler that captured the session keeps both alive
  // forever otherwise. See PeerSession::ReleaseHandler.
  if (client_session_) {
    client_session_->ReleaseHandler();
  }
}

Result Client::Bind() {
  Result init_result = Init();
  if (ZNET_UNLIKELY(init_result != Result::Success)) ZNET_UNLIKELY_ATTR {
    ZNET_LOG_ERROR("Cannot bind because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return init_result;
  }
  if (ZNET_UNLIKELY(!backend_)) ZNET_UNLIKELY_ATTR {
    return Result::InvalidBackend;
  }
  return backend_->Bind();
}

Result Client::Bind(const std::string& ip, PortNumber port) {
  Result init_result = Init();
  if (ZNET_UNLIKELY(init_result != Result::Success)) ZNET_UNLIKELY_ATTR {
    ZNET_LOG_ERROR("Cannot bind because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return init_result;
  }
  if (ZNET_UNLIKELY(!backend_)) ZNET_UNLIKELY_ATTR {
    return Result::InvalidBackend;
  }
  return backend_->Bind(ip, port);
}

Result Client::Connect() {
  if (task_.IsRunning()) {
    return Result::AlreadyConnected;
  }
  if (ZNET_UNLIKELY(!backend_)) ZNET_UNLIKELY_ATTR {
    return Result::InvalidBackend;
  }
  // registered before Connect() starts any receive thread, which reads it
  auto signal = signal_;
  backend_->SetWakeCallback([signal]() { signal->Raise(); });

  Result result = backend_->Connect();
  if (ZNET_UNLIKELY(result != Result::Success)) ZNET_UNLIKELY_ATTR {
    return result;
  }

  client_session_ = backend_->client_session();
  // takes over the session's wake callback too, so start it before the session
  // reaches the application
  encoder_.Start(client_session_, [signal]() { signal->Raise(); });
  // only worth pacing when something else is taking datagrams off the socket.
  // otherwise reads happen only while this loop runs, and sleeping would add a
  // tick to everything inbound.
  const bool paced = backend_->DrivesOwnReceive();

  task_.Run([this, paced]() {
    signal_->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    // held only around the wait, never across Process(): the notifiers take it
    // too, so holding it while working would block the receive thread for a
    // whole tick. nothing else is guarded by it.
    std::unique_lock<std::mutex> lock(signal_->mutex, std::defer_lock);
    auto rest_of_tick = [this, paced, &lock]() {
      if (!paced) {
        return;  // this loop is the only reader; sleeping would stall inbound
      }
      scheduler_.End();
      auto remaining = scheduler_.remaining();
      lock.lock();
      if (remaining > Scheduler::Duration::zero()) {
        signal_->cv.wait_for(lock, remaining, [this]() {
          return signal_->woken.load(std::memory_order_relaxed) ||
                 task_.IsStopRequested();
        });
      }
      signal_->woken.store(false, std::memory_order_relaxed);
      lock.unlock();
    };
    // setup
    while (!client_session_->IsReady() && client_session_->IsAlive() && !task_.IsStopRequested()) {
      scheduler_.Start();
      client_session_->Process();
      if (config_.connection_timeout.count() > 0 && client_session_->time_since_connect() > config_.connection_timeout) {
        ZNET_LOG_DEBUG("Connection to {} timed-out.", server_address_->readable());
        client_session_->Close();
      }
      rest_of_tick();
    }
    if (!client_session_->IsAlive() || task_.IsStopRequested()) {
      return;
    }
    ZNET_LOG_DEBUG("Connected to the server.");
    ClientConnectedToServerEvent connected_event{client_session_};
    event_callback()(connected_event);
    while (client_session_->IsAlive() && !task_.IsStopRequested()) {
      scheduler_.Start();
      client_session_->Process();
      rest_of_tick();
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

Result Client::Disconnect(CloseOptions options) {
  if (!client_session_) {
    return Result::Failure;
  }
  Result result = client_session_->Close(options);
  signal_->Raise();  // sleeping out a tick; do not make it wait
  encoder_.Stop();
  return result;
}

ZNET_NODISCARD std::shared_ptr<InetAddress> Client::local_address() const {
  return backend_->local_address();
}

}  // namespace znet