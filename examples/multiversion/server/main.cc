//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <algorithm>
#include <unordered_map>
#include "packets.h"
#include "znet/znet.h"

using namespace znet;

Codecs codecs_;

class Player {
 public:
  Player() {}

 public:
  int protocol_ = 0;
  Vec3 pos_;
};

struct PlayingPacketHandler
    : public PacketHandler<PlayingPacketHandler, MovePacket> {

  PlayingPacketHandler(std::shared_ptr<PeerSession> session, std::shared_ptr<Player> player)
        : session_(session), player_(player) {}

  void OnPacket(const MovePacket& pk) {
    player_->pos_ += pk.delta;
  }

 private:
  std::shared_ptr<PeerSession> session_;
  std::shared_ptr<Player> player_;
};

struct LoginPacketHandler
    : public PacketHandler<LoginPacketHandler, NetworkSettingsPacket,
                           ClientReadyPacket> {
 public:
  LoginPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  void OnPacket(const NetworkSettingsPacket& pk) {
    std::shared_ptr<Player> player = session_->user_ptr_typed<Player>();
    if (player) {
      player->protocol_ = pk.protocol_;
      ZNET_LOG_INFO("Player protocol set to: {}", player->protocol_);
    } else {
      ZNET_LOG_ERROR("User object is not a Player type for session!");
    }

    if (pk.protocol_ == 1) {
      session_->SetCodec(codecs_.codec_v1);
    } else if (pk.protocol_ == 2) {
      session_->SetCodec(codecs_.codec_v2);
    } else {
      ZNET_LOG_ERROR("Invalid protocol version: {}", pk.protocol_);
      session_->Close();
      return;
    }

    auto start_game = std::make_shared<StartGamePacket>();
    start_game->spawn_pos_ = {0, 60, 0};
    start_game->game_mode_ = 0;
    start_game->level_name_ = "test_world";
    session_->SendPacket(start_game);
  }

  void OnPacket(const ClientReadyPacket& pk) {
    ZNET_LOG_INFO("Client ready {}!", session_->id());
    session_->SetHandler(std::make_shared<PlayingPacketHandler>(session_,
                                                                session_->user_ptr_typed<Player>()));
    // After this, session_ is invalid.
  }

 private:
  std::shared_ptr<PeerSession> session_;
};


std::vector<std::shared_ptr<Player>> active_players_;

// Called whenever a new client connects to the server
// Sets up the communication channel with proper encoding and handling
bool OnNewSessionEvent(ServerClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(codecs_.codec_latest); // Initial codec
  session.SetHandler(std::make_shared<LoginPacketHandler>(event.session())); // initial handler
  std::shared_ptr<Player> player = std::make_shared<Player>();
  session.SetUserPointer(player);
  active_players_.push_back(std::move(player));
  return true;
}

bool OnDisconnectSessionEvent(znet::ServerClientDisconnectedEvent& event) {
  znet::PeerSession& session = *event.session();
  std::shared_ptr<Player> user_ptr = session.user_ptr_typed<Player>();
  if (!user_ptr) {
    return false;
  }
  auto it = std::find(active_players_.begin(), active_players_.end(), user_ptr);
  if (it != active_players_.end()) {
    ZNET_LOG_INFO("Player disconnected. Removing.");
    std::iter_swap(it, active_players_.end() - 1);
    active_players_.pop_back();
  }
  return false;
}

// Central event dispatcher that routes all server events
// to their appropriate handler functions
void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  // Route for different types of events
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnNewSessionEvent));
  dispatcher.Dispatch<ServerClientDisconnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnectSessionEvent));
}

int main() {
  Result result;

  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // Create the server configuration
  // We're listening on localhost (127.0.0.1) port 25000
  // In a real application, you'd typically get these values from
  // command line arguments or a config file or from ui
  ServerConfig config{"localhost", 25000};

  // Initialize the server with our configuration
  // This sets up the internal server state but doesn't start listening yet
  Server server{config};

  // Set up signal handling for graceful shutdown
  // This ensures the server closes cleanly when interrupted (Ctrl+C)
  // This part is optional
  RegisterSignalHandler([&server](Signal sig) -> bool {
    // stop the server when SIGINT is received
    server.Stop();
    return server.shutdown_complete();
  }, znet::kSignalInterrupt);

  // Register our event handler to process server events
  // OnEvent will be called for client connections, disconnections,
  // and other server events
  server.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  // Try to bind the server to the configured network interface
  // This reserves the port for our use
  if ((result = server.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }

  // Start listening for incoming client connections
  // This begins accepting clients but doesn't block the main thread (async)
  if ((result = server.Listen()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to listen: {}", GetResultString(result));
    return 1;
  }

  // Wait for the server to stop
  // Note: In a real application, you typically wouldn't block here
  // Instead, you'd continue your program and do other stuff
  server.Wait();

  // Server has shut down cleanly
  znet::Cleanup();
  return 0;
}
