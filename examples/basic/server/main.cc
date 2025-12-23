//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/init.h"
#include "znet/server_events.h"
#include "znet/codec.h"
#include "znet/server.h"
#include "znet/signal_handler.h"

#include "packets.h"

#include <iostream>

using namespace znet;

// MyPacketHandler manages network communication, handling incoming messages
// from connected clients and sending appropriate responses
class MyPacketHandler : public PacketHandler<MyPacketHandler, DemoPacket> {
 public:
  // Creates a new handler for a specific client session
  MyPacketHandler(std::shared_ptr<PeerSession> session) : session_(session) { }

  // Processes incoming demo packets from clients
  // Responds with a greeting message to demonstrate two-way communication
  void OnPacket(std::shared_ptr<DemoPacket> p) {
    ZNET_LOG_INFO("Received demo_packet.");
    std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
    pk->text = "Got ya! Hello from server!";
    session_->SendPacket(pk);
  }

 private:
  // The client session this handler is responsible for
  std::shared_ptr<PeerSession> session_;
};

// Called whenever a new client connects to the server
// Sets up the communication channel with proper encoding and handling
bool OnNewSessionEvent(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  // Create and set up the packet codec (encoder/decoder)
  // In production, you'd typically share one codec instance among all clients
  // rather than creating a new one for each connection
  std::shared_ptr<Codec> codec = std::make_shared<Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  session.SetCodec(codec);

  // Set up the packet handler for this client
  // Different handlers can be used for different connection states
  // (e.g., one for login, another for gameplay)
  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));
  return false;
}

// Called when a client disconnects from the server
// You can add cleanup code here if needed
bool OnDisconnectSessionEvent(ServerClientDisconnectedEvent& event) {
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
  ServerConfig config{"localhost", 25000, std::chrono::seconds(10)};

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