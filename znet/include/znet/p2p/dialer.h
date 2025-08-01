//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PARENT_DIALER_H
#define ZNET_PARENT_DIALER_H

#include "znet/interface.h"
#include "znet/client.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/client_events.h"
#include <optional>

namespace znet {
namespace holepunch {

enum class DialResult {
  SuccessHost,
  SuccessClient,
  FailureTimeout
};

struct DialerConfig {
  std::string local_ip;
  PortNumber local_port;
  std::string peer_ext_ip;
  PortNumber peer_ext_port;
  ConnectionType connection_type;
};

class DialerCompleteWithServerEvent : public Event {
 public:
  explicit DialerCompleteWithServerEvent(std::shared_ptr<Server> server)
      : server_(server) {}

  std::shared_ptr<Server> server() { return server_; }

  ZNET_EVENT_CLASS_TYPE(DialerCompleteWithServerEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  std::shared_ptr<Server> server_;
};

class DialerCompleteWithClientEvent : public Event {
 public:
  explicit DialerCompleteWithClientEvent(std::shared_ptr<Client> client)
      : client_(client) {}

  std::shared_ptr<Client> client() { return client_; }

  ZNET_EVENT_CLASS_TYPE(DialerCompleteWithClientEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  std::shared_ptr<Client> client_;
};


class Dialer : public Interface {
 public:
  Dialer(const DialerConfig&);
  ~Dialer();

  Result Bind() override;

  void Wait() override;

  Result Dial();

  void SetEventCallback(EventCallbackFn fn) override {
    event_callback_ = std::move(fn);
    server_->SetEventCallback(event_callback_);
    client_->SetEventCallback(event_callback_);
  }

  void OnServerEvent(Event&);
  void OnClientEvent(Event&);

  bool OnEvent(ClientConnectedToServerEvent& event);
  bool OnEvent(IncomingClientConnectedEvent& event);
 private:
  DialerConfig config_;
  std::shared_ptr<Server> server_;
  std::shared_ptr<Client> client_;
};

}
}
#endif  //ZNET_PARENT_DIALER_H
