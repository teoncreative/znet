
//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <thread>
#include "events.h"
#include "znet/znet.h"

using namespace znet;

class User;

class ChatServer {
 public:
  ChatServer();

  void Start();

  void OnEvent(Event& event);

  void BroadcastPacket(Ref<Packet> packet);

 private:
  Server server_;
  std::unordered_map<int, Ref<User>> connected_users_;
  std::unordered_map<int, Ref<User>> pending_users_;
  std::unordered_map<Ref<InetAddress>, int> users_ids_by_addr_;
  int user_id_counter = 0;
  Ref<std::thread> server_thread_;

 private:
  bool OnNewSessionEvent(ServerClientConnectedEvent& event);
  bool OnServerClientDisconnectedEvent(ServerClientDisconnectedEvent& event);
  bool OnUserAuthorizedEvent(UserAuthorizedEvent& event);
};
