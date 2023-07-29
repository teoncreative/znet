//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "user.h"


User::User(ChatServer& server, int user_id, Ref<znet::ConnectionSession> session) : server_(server), user_id_(user_id), session_(session) {
  HandlerLayer& handler = session->handler_layer();

  auto login_request_packet =
      CreateRef<PacketHandler<LoginRequestPacket,
                              LoginRequestPacketSerializerV1>>();
  login_request_packet->AddReceiveCallback(ZNET_BIND_FN(OnLoginRequest));
  handler.AddPacketHandler(login_request_packet);

  auto login_response_packet =
      CreateRef<PacketHandler<LoginResponsePacket,
                              LoginResponsePacketSerializerV1>>();
  handler.AddPacketHandler(login_response_packet);

  auto server_settings_packet =
      CreateRef<PacketHandler<ServerSettingsPacket,
                              ServerSettingsPacketSerializerV1>>();
  handler.AddPacketHandler(server_settings_packet);

  auto message_packet =
      CreateRef<PacketHandler<MessagePacket, MessagePacketSerializerV1>>();
  message_packet->AddReceiveCallback(ZNET_BIND_FN(OnMessage));
  handler.AddPacketHandler(message_packet);
}

void User::SendPacket(Ref<znet::Packet> packet) {
  session_->SendPacket(packet);
}

void User::OnLoginRequest(znet::ConnectionSession& session, Ref<LoginRequestPacket> packet) {
  ZNET_LOG_DEBUG("Received login request");
  username_ = packet->username_;
  password_ = packet->password_;
  // check registration
  auto login_response = CreateRef<LoginResponsePacket>();
  login_response->succeeded_ = true;
  login_response->message_ = "Welcome to zchat!";
  login_response->user_id_ = user_id_;
  SendPacket(login_response);
  auto server_settings = CreateRef<ServerSettingsPacket>();
  SendPacket(server_settings);
  login_complete_ = true;

  UserAuthorizedEvent event{*this};
  server_.OnEvent(event);
}

void User::OnMessage(znet::ConnectionSession& session, Ref<MessagePacket> packet) {
  if (!login_complete_) {
    return;
  }
  packet->sender_username_ = username_;
  packet->user_id_ = user_id_;
  ZNET_LOG_INFO("{}: {}", packet->sender_username_, packet->message_);
  server_.BroadcastPacket(packet);
}