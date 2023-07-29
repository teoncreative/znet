//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <iostream>
#include <queue>
#include <thread>
#include "packets.h"
#include "znet/znet.h"

using namespace znet;

PacketId expected_packet_ = 0;
bool received_login_ = false;
bool received_settings_ = false;
int user_id_;

bool OnClientConnect(ClientConnectedToServerEvent& event) {
  std::cout << "Connection successful!" << std::endl;

  HandlerLayer& handler = event.session()->handler_layer();

  auto login_request_packet = CreateRef<
      PacketHandler<LoginRequestPacket, LoginRequestPacketSerializerV1>>();
  handler.AddPacketHandler(login_request_packet);

  auto login_response_packet = CreateRef<
      PacketHandler<LoginResponsePacket, LoginResponsePacketSerializerV1>>();
  login_response_packet->AddReceiveCallback(
      [](ConnectionSession& session, Ref<LoginResponsePacket> packet) {
        if (received_login_ ||
            (expected_packet_ != 0 && expected_packet_ != packet->id())) {
          ZNET_LOG_DEBUG(
              "Received packet LoginResponsePacket but it was not expected!");
          session.Close();
          return true;
        }
        if (!packet->succeeded_) {
          std::cout << "Login was not successful!" << std::endl;
          if (!packet->message_.empty()) {
            std::cout << packet->message_ << std::endl << std::endl;
          }
          session.Close();
          return true;
        }
        std::cout << "Login was successful!" << std::endl;
        if (!packet->message_.empty()) {
          std::cout << packet->message_ << std::endl << std::endl;
        }
        // Update the token
        user_id_ = packet->user_id_;
        expected_packet_ = ServerSettingsPacket::PacketId();
        received_login_ = true;
        return false;
      });
  handler.AddPacketHandler(login_response_packet);

  auto server_settings_packet = CreateRef<
      PacketHandler<ServerSettingsPacket, ServerSettingsPacketSerializerV1>>();
  server_settings_packet->AddReceiveCallback(
      [](ConnectionSession& session, Ref<ServerSettingsPacket> packet) {
        if (received_settings_ ||
            (expected_packet_ != 0 && expected_packet_ != packet->id())) {
          ZNET_LOG_DEBUG(
              "Received packet ServerSettingsPacket but it was not expected!");
          session.Close();
          return true;
        }

        expected_packet_ = 0;
        received_settings_ = true;
        return false;
      });
  handler.AddPacketHandler(server_settings_packet);

  auto message_packet =
      CreateRef<PacketHandler<MessagePacket, MessagePacketSerializerV1>>();
  message_packet->AddReceiveCallback([](ConnectionSession& session,
                                        Ref<MessagePacket> packet) {
    if (expected_packet_ != 0) {
      ZNET_LOG_DEBUG("Received packet MessagePacket but it was not expected!");
      session.Close();
      return true;
    }

    std::cout << packet->sender_username_ << ": " << packet->message_
              << std::endl;
    return false;
  });
  handler.AddPacketHandler(message_packet);

  auto packet = CreateRef<LoginRequestPacket>();
  std::string username;
  std::string password;

  std::cout << "Username: ";
  std::cin >> packet->username_;
  std::cout << "Password: ";
  std::cin >> packet->password_;

  event.session()->SendPacket(packet);
  expected_packet_ = LoginResponsePacket::PacketId();
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};

  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnClientConnect));
}

int main() {
  while (true) {
    std::cout << "Please enter server address: ";
    std::string ip;
    std::cin >> ip;

    ClientConfig config{ip, 25000};

    Ref<Client> client = CreateRef<Client>(config);
    client->SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));
    std::thread thread = std::thread([client]() {
      // Bind and listen
      client->Bind();
      client->Connect();
    });
    thread.detach();

    while (true) {
      if (!received_settings_) {
        continue;
      }
      std::string in;
      std::cin >> in;
      if (in == "?quit") {
        client->Disconnect();
        return 0;
      }
      if (in.empty()) {
        continue;
      }
      auto session = client->client_session();
      Ref<MessagePacket> message = CreateRef<MessagePacket>();
      message->message_ = in;
      session->SendPacket(message);
    }
  }
}
