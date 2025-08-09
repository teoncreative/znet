//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <iostream>
#include "packets.h"
#include "znet/znet.h"

using namespace znet;


#include <iostream>
#include "packets.h"
#include "znet/znet.h"
#include "player.h"

using namespace znet;

Codecs codecs_;
std::unique_ptr<Player> player;

struct PlayingPacketHandler
    : public PacketHandler<PlayingPacketHandler, MovePacket> {

  PlayingPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  void OnPacket(const TeleportPacket& pk) {
    player->pos_ = pk.pos;
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

struct LoginPacketHandler
    : public PacketHandler<LoginPacketHandler,
                           StartGamePacket> {
 public:
  LoginPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  void OnPacket(const StartGamePacket& pk) {
    // Here, the pk.spawn_pos_ is available and sent by the server.
    ZNET_LOG_INFO("Game start! LevelName: {}, GameMode: {}, SpawnPos: {}", pk.level_name_, pk.game_mode_, pk.spawn_pos_.to_string());
    session_->SendPacket(std::make_shared<ClientReadyPacket>());
    session_->SetHandler(std::make_shared<PlayingPacketHandler>(session_));
  }

 private:
  std::shared_ptr<PeerSession> session_;
};


bool OnConnectEvent(ClientConnectedToServerEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(codecs_.codec_v2);

  session.SetHandler(std::make_shared<LoginPacketHandler>(event.session()));

  std::shared_ptr<NetworkSettingsPacket> pk = std::make_shared<NetworkSettingsPacket>();
  pk->protocol_ = 2;
  event.session()->SendPacket(pk);
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
}

int main() {
  Result result;

  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // Create a client configuration
  // We're connecting to localhost (127.0.0.1) on port 25000
  // In a real application, you'd typically get these values from
  // command line arguments or a config file or from ui
  ClientConfig config{"localhost", 25000};

  // Initialize the network client with our configuration
  // This sets up the internal client state but doesn't connect yet
  Client client{config};

  // Register our event handler to process network events
  // OnEvent will be called for events.
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  // Try to bind the client to a local network interface
  // This is required before we can connect to the server
  if ((result = client.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }

  // Attempt to establish a connection to the server
  // This initiates the connection process but doesn't wait. (async)
  // It runs on another thread
  if ((result = client.Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to connect: {}", GetResultString(result));
    return 1;
  }

  // Wait for the connection to complete
  // Note: In a real application, you typically wouldn't block here
  // Instead, you'd continue your program and do other stuff
  client.Wait();

  // Connection is finished (disconnected)
  znet::Cleanup();
  return 0;
}
