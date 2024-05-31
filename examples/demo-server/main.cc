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

std::vector<Ref<DemoPacket>> packets;

void OnDemoPacket(PeerSession& session, Ref<DemoPacket> packet) {
  ZNET_LOG_INFO("Received demo_packet.");
  Ref<DemoPacket> pk = CreateRef<DemoPacket>();
  for (int i = 0; i < 4000; ++i) {
    pk->text[i] = 'a' + (i % 26);
  }
  session.SendPacket(pk);
  packets.push_back(packet);
}

void AddClientHandlers(Ref<PeerSession> session) {
  auto demo_packet_handler =
      CreateRef<PacketHandler<DemoPacket, DemoPacketSerializerV1>>();
  demo_packet_handler->AddReceiveCallback(ZNET_BIND_GLOBAL_FN(OnDemoPacket));
  session->handler_layer().AddPacketHandler(demo_packet_handler);
}

bool OnNewSessionEvent(ServerClientConnectedEvent& event) {
  AddClientHandlers(event.session());
  return false;
}

bool OnDisconnectSessionEvent(ServerClientDisconnectedEvent& event) {
  packets.clear();
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