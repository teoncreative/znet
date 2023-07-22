//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>  // For memset
#include <iostream>
#include "packets.h"
#include "znet/znet.h"

using namespace znet;

void OnDemoPacket(ConnectionSession& session, Ref<DemoPacket> packet) {
  ZNET_LOG_INFO("Received demo_packet. Text: {}", packet->text);
}

void AddClientHandlers(Ref<ConnectionSession> session) {
  auto demo_packet_handler =
      CreateRef<PacketHandler<DemoPacket, DemoPacketSerializerV1>>();
  demo_packet_handler->AddReceiveCallback(ZNET_BIND_GLOBAL_FN(OnDemoPacket));
  session->handler_layer().AddPacketHandler(demo_packet_handler);
}

bool OnConnectEvent(ClientConnectedToServerEvent& event) {
  AddClientHandlers(event.session());
  Ref<DemoPacket> pk = CreateRef<DemoPacket>();
  pk->text = "Hello from client!";
  event.session()->SendPacket(pk);
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
}

int main() {
  // Create config
  ClientConfig config{"127.0.0.1", 25000};

  // Create client with the config
  Client client{config};

  // Set event callback
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  // Bind and connect
  client.Bind();

  // If connection is successful, this function will block the flow
  // of the program until it disconnects.
  if (!client.Connect()) {
    return 1; // Failed to connect
  }

  // Connection was successful and completed.
  return 0;
}
