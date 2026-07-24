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
// ZDT example server. Identical in shape to examples/basic, but the config uses
// ConnectionType::ZDT so the whole session runs over the reliable-UDP transport.
//

#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/signal_handler.h"

#include "packets.h"

using namespace znet;

// Echoes every DemoPacket it receives back to the sender.
class EchoHandler : public PacketHandler<EchoHandler, DemoPacket> {
 public:
  explicit EchoHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(std::shared_ptr<DemoPacket> packet) {
    ZNET_LOG_INFO("[server] received: {}", packet->text);
    auto reply = std::make_shared<DemoPacket>();
    reply->text = "echo: " + packet->text;
    session_->SendPacket(reply);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnNewSession(IncomingClientConnectedEvent& event) {
  ZNET_LOG_INFO("[server] client connected over ZDT: {}",
                event.session()->remote_address()->readable());
  auto codec = std::make_shared<Codec>();
  codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
  event.session()->SetCodec(codec);
  event.session()->SetHandler(std::make_shared<EchoHandler>(event.session()));
  return false;
}

bool OnDisconnect(ServerClientDisconnectedEvent& event) {
  ZNET_LOG_INFO("[server] client disconnected: {}",
                event.session()->remote_address()->readable());
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnNewSession));
  dispatcher.Dispatch<ServerClientDisconnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnect));
}

int main() {
  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  ServerConfig config{"127.0.0.1", 25000, std::chrono::seconds(10),
                      ConnectionType::ZDT};
  Server server{config};

  RegisterSignalHandler(
      [&server](Signal) -> bool {
        server.Stop();
        return server.shutdown_complete();
      },
      znet::kSignalInterrupt);

  server.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = server.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }
  if ((result = server.Listen()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to listen: {}", GetResultString(result));
    return 1;
  }
  ZNET_LOG_INFO("[server] ZDT server listening on 127.0.0.1:25000 (Ctrl+C to stop)");

  server.Wait();
  znet::Cleanup();
  return 0;
}
