//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// The client half of the user-pointer example. It introduces itself with a
// HelloPacket, then sends a few messages so the server has something to count.
//
// A client has one session, so it has less need for a user pointer than a
// server does: state can simply live in the handler. The mechanism is
// identical if you want it, though, and this file attaches one to show that.
//

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"

#include "packets.h"

using namespace znet;

// how many messages the client sends, and so how many replies it waits for
constexpr uint32_t kExpectedReplies = 3;

struct SessionStats {
  uint32_t replies_received = 0;
};

class MyPacketHandler : public PacketHandler<MyPacketHandler, MessagePacket> {
 public:
  explicit MyPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(std::shared_ptr<MessagePacket> packet) {
    ZNET_LOG_INFO("Server said: {}", packet->text);

    std::shared_ptr<SessionStats> stats = session_->user_pointer<SessionStats>();
    if (!stats) {
      return;
    }
    stats->replies_received++;

    // The state attached to the session is what decides when we are done, so
    // the handler needs no counter of its own. Closing here lets Wait() return
    // and the server see a real disconnect rather than a dead peer.
    if (stats->replies_received == kExpectedReplies) {
      session_->Close();
    }
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnConnectEvent(ClientConnectedToServerEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(MakeCodec());
  session.SetUserPointer(std::make_shared<SessionStats>());
  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));

  auto hello = std::make_shared<HelloPacket>();
  hello->name = "example-client";
  session.SendPacket(hello);

  for (uint32_t i = 1; i <= kExpectedReplies; i++) {
    auto message = std::make_shared<MessagePacket>();
    message->text = "hello number " + std::to_string(i);
    session.SendPacket(message);
  }
  return false;
}

bool OnDisconnectEvent(ClientDisconnectedFromServerEvent& event) {
  std::shared_ptr<SessionStats> stats =
      event.session()->user_pointer<SessionStats>();
  if (stats) {
    ZNET_LOG_INFO("Disconnected after {} reply/replies.",
                  stats->replies_received);
  }
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnectEvent));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnectEvent));
}

int main() {
  Result result;

  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  ClientConfig config{"localhost", 25000};
  Client client{config};
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = client.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }

  if ((result = client.Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to connect: {}", GetResultString(result));
    return 1;
  }

  client.Wait();

  znet::Cleanup();
  return 0;
}
