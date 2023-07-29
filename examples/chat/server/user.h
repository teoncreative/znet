
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

#include "packets.h"
#include "chat_server.h"

class User {
 public:
  User(ChatServer& server, int user_id, Ref<ConnectionSession> session);

  void SendPacket(Ref<Packet> packet);

  int user_id() const { return user_id_; }
 private:
  ChatServer& server_;
  Ref<znet::ConnectionSession> session_;
  bool login_complete_ = false;
  int sent_messages_ = 0;
  int received_messages_ = 0;
  std::string username_;
  std::string password_;
  int user_id_;

 private:
  void OnLoginRequest(ConnectionSession& session, Ref<LoginRequestPacket> packet);
  void OnMessage(ConnectionSession& session, Ref<MessagePacket> packet);
};