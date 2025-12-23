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
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/client.h"

#include "packets.h"

#include <iostream>

using namespace znet;

// MyPacketHandler manages network communication with a simple request-response pattern.
// It inherits from PacketHandler and specifically handles DemoPacket type messages.
class MyPacketHandler : public PacketHandler<MyPacketHandler, DemoPacket> {
 public:
  // Creates a new handler associated with the given peer session
  MyPacketHandler(std::shared_ptr<PeerSession> session) : session_(session) { }

  // Called when a demo packet is received. Logs the event and sends back
  // a response with a greeting message.
  void OnPacket(std::shared_ptr<DemoPacket> p) {
    ZNET_LOG_INFO("Received demo_packet.");
    std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
    pk->text = "Got ya! Hello from server!";
    session_->SendPacket(pk);
  }

 private:
  // Stores the session we use to send responses back to the client
  std::shared_ptr<PeerSession> session_;
};

// Sets up a new client connection with appropriate packet handling and encoding.
// Returns false to allow other handlers to process the event if needed.
bool OnConnectEvent(ClientConnectedToServerEvent& event) {
  PeerSession& session = *event.session();

  // Set up how packets will be encoded/decoded
  // Note: It's more efficient to create this codec once and share it
  // between clients, but for this example we create it per-connection

  std::shared_ptr<Codec> codec = std::make_shared<Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  session.SetCodec(codec);

  // Set up how packets will be processed
  // The handler can be changed during the session - for example,
  // you might use different handlers for login vs. game play
  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));

  // Send an initial greeting to the other peer
  std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
  pk->text = "Hello from client!";
  event.session()->SendPacket(pk);
  return false;
}

// Main event dispatcher - routes different types of events
// to their appropriate handlers
void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  // Route for different types of events
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
