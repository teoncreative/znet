//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "chat_server.h"
#include "user.h"

ChatServer::ChatServer() {
  // Create config
  ServerConfig config{"172.24.222.44", 25000};

  // Create server with the config
  server_ = {config};

  // Register signal handlers (optional)
  RegisterSignalHandler([this](Signal sig) -> bool {
    if (sig == znet::kSignalInterrupt) {
      // stop the server when SIGINT is received
      server_.Stop();
      return server_.shutdown_complete();
    }
    return false;
  });

  // Set event callback
  server_.SetEventCallback(ZNET_BIND_FN(OnEvent));
}

void ChatServer::Start() {
  server_thread_ = CreateRef<std::thread>([this]() {
    // Bind and listen
    server_.Bind();
    server_.Listen();
  });
  server_thread_->detach();

  while (true) {
    std::string in;
    std::cin >> in;
    if (in == "?stop") {
      server_.Stop();
      break;
    }
  }
}

void ChatServer::OnEvent(znet::Event& event) {
  EventDispatcher dispatcher{event};

  dispatcher.Dispatch<ServerClientConnectedEvent>(
      ZNET_BIND_FN(OnNewSessionEvent));
  dispatcher.Dispatch<ServerClientDisconnectedEvent>(
      ZNET_BIND_FN(OnServerClientDisconnectedEvent));
  dispatcher.Dispatch<UserAuthorizedEvent>(ZNET_BIND_FN(OnUserAuthorizedEvent));
}

void ChatServer::BroadcastPacket(Ref<znet::Packet> packet) {
  for (const auto& item : connected_users_) {
    item.second->SendPacket(packet);
  }
}

bool ChatServer::OnNewSessionEvent(ServerClientConnectedEvent& event) {
  Ref<User> user = CreateRef<User>(*this, user_id_counter++, event.session());
  pending_users_[user->user_id()] = user;
  users_ids_by_addr_[event.session()->remote_address()] = user->user_id();
  return false;
}

bool ChatServer::OnServerClientDisconnectedEvent(
    znet::ServerClientDisconnectedEvent& event) {
  int user_id = users_ids_by_addr_[event.session()->remote_address()];
  Ref<User> user = connected_users_[user_id];
  connected_users_.erase(user->user_id());
  pending_users_.erase(user->user_id());
  users_ids_by_addr_.erase(event.session()->remote_address());
  ZNET_LOG_DEBUG("Disconnected user {}", user_id);
  return false;
}

bool ChatServer::OnUserAuthorizedEvent(UserAuthorizedEvent& event) {
  Ref<User> user = pending_users_[event.user().user_id()];
  pending_users_.erase(user->user_id());
  connected_users_[user->user_id()] = user;
  return false;
}
