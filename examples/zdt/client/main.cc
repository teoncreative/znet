//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// ZDT example client. Connects to the ZDT server, then sends one numbered
// "ping" per second (reliable + ordered by default) and logs each echo. Watching
// the numbers arrive in order demonstrates reliable delivery over UDP.
//

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"

#include "packets.h"

#include <chrono>
#include <thread>

using namespace znet;

// Logs each echo the server sends back.
class ReplyHandler : public PacketHandler<ReplyHandler, DemoPacket> {
 public:
  void OnPacket(std::shared_ptr<DemoPacket> packet) {
    ZNET_LOG_INFO("[client] got {}", packet->text);
  }
};

bool OnConnected(ClientConnectedToServerEvent& event) {
  ZNET_LOG_INFO("[client] connected to server over ZDT");
  auto session = event.session();

  auto codec = std::make_shared<Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  session->SetCodec(codec);
  session->SetHandler(std::make_shared<ReplyHandler>());

  // Paced sender: one numbered packet per second while the session is alive.
  // Capturing `session` keeps it alive for the thread's lifetime.
  std::thread([session]() {
    int counter = 0;
    while (session->IsAlive()) {
      auto packet = std::make_shared<DemoPacket>();
      packet->text = "ping #" + std::to_string(++counter);
      if (!session->SendPacket(packet)) {
        break;
      }
      ZNET_LOG_INFO("[client] sent {}", packet->text);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();
  return false;
}

bool OnDisconnected(ClientDisconnectedFromServerEvent&) {
  ZNET_LOG_INFO("[client] disconnected from server");
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnected));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnected));
}

int main() {
  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  ClientConfig config{"127.0.0.1", 25000, std::chrono::seconds(10),
                      ConnectionType::ZDT};
  Client client{config};
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = client.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }
  ZNET_LOG_INFO("[client] connecting to 127.0.0.1:25000 over ZDT...");
  if ((result = client.Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to connect: {}", GetResultString(result));
    return 1;
  }

  client.Wait();
  znet::Cleanup();
  return 0;
}
