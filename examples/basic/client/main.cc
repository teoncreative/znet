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

bool OnConnectEvent(ClientConnectedToServerEvent& event) {
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

  std::shared_ptr<DemoPacket> pk = std::make_shared<DemoPacket>();
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
  if (client.Bind() != Result::Success) {
    return 1;
  }

  if (client.Connect() != Result::Success) {
    return 1;  // Failed to connect
  }
  client.Wait();

  // Connection was successful and completed.
  return 0;
}
