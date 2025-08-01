//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/dialer.h"

namespace znet {
namespace holepunch {

Dialer::Dialer(const DialerConfig& config) : config_(config) {
  server_ = std::make_shared<Server>(ServerConfig{config.local_ip, config.local_port});
  server_->SetEventCallback(ZNET_BIND_FN(OnServerEvent));
  client_ = std::make_shared<Client>(ClientConfig{config.peer_ext_ip, config.peer_ext_port});
  client_->SetEventCallback(ZNET_BIND_FN(OnClientEvent));
}

Dialer::~Dialer() {

}

Result Dialer::Bind() {
  Result result;
  if ((result = server_->Bind()) != Result::Success) {
    return result;
  }
  if ((result = client_->Bind(config_.local_ip, config_.local_port)) != Result::Success) {
    return result;
  }
  return Result::Success;
}

Result Dialer::Dial() {
  Result result;
  if ((result = server_->Listen()) != Result::Success) {
    return result;
  }
  if ((result = client_->Connect()) != Result::Success) {
    return result;
  }
  return Result::Success;
}

void Dialer::Wait() {
  client_->Wait();
  server_->Wait();
}

void Dialer::OnServerEvent(znet::Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(ZNET_BIND_FN(OnEvent));
}

void Dialer::OnClientEvent(znet::Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(ZNET_BIND_FN(OnEvent));
}

bool Dialer::OnEvent(IncomingClientConnectedEvent& event) {
  client_->Disconnect();
  client_ = nullptr;
  DialerCompleteWithServerEvent complete_event{server_};
  event_callback_(complete_event);
  return false;
}

bool Dialer::OnEvent(ClientConnectedToServerEvent& event) {
  server_->Stop();
  server_ = nullptr;
  DialerCompleteWithClientEvent complete_event{client_};
  event_callback_(complete_event);
  return false;
}

}
}