//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "packets.h"
#include "znet/znet.h"

using namespace znet;

class MyPacketHandler : public PacketHandler<MyPacketHandler, DemoPacket> {
 public:
  MyPacketHandler(std::shared_ptr<PeerSession> session) : session_(session) { }

  void OnPacket(std::shared_ptr<DemoPacket> p) {
    ZNET_LOG_INFO("Received demo_packet.");
    std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
    pk->text = "Got ya! Hello from server!";
    session_->SendPacket(pk);
  }

  void OnUnknown(std::shared_ptr<Packet> p) {
    // fallback for unknown types
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnNewSessionEvent(ServerClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  // Codecs are what serializes and deserializes the packets,
  // You can set an initial codec, then get the protocol version from the client
  // then set a different codec for different versions. This provides much more flexibility
  // Codecs can be swapped mid-session

  // Additionally, it is wiser to have this created once and shared between clients
  // For this example it is created on the spot like this
  std::shared_ptr<Codec> codec = std::make_shared<Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  session.SetCodec(codec);

  // Handlers are what handles the packets, like codecs this can be swapped mid-session
  // For example during the login phase, you can give a different handler to only
  // handle those are sent during the login, then change the codec.
  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));
  return false;
}

bool OnDisconnectSessionEvent(ServerClientDisconnectedEvent& event) {
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  // Bind desired events
  dispatcher.Dispatch<ServerClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnNewSessionEvent));
  dispatcher.Dispatch<ServerClientDisconnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnectSessionEvent));
}

int main() {
  // Create config
  ServerConfig config{"127.0.0.1", 25000};

  // Create server with the config
  Server server{config};

  // Register signal handlers (optional)
  RegisterSignalHandler([&server](Signal sig) -> bool {
    if (sig == znet::kSignalInterrupt) {
      // stop the server when SIGINT is received
      server.Stop();
      return server.shutdown_complete();
    }
    return false;
  });

  // Set event callback
  server.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  // Bind and listen
  if (server.Bind() != Result::Success) {
    return 1;
  }

  if (server.Listen() != Result::Success) {
    return 1; // failed to listen
  }
  server.Wait();
  return 0; // successfully completed
}