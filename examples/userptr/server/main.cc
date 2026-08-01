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
// Attaching per-connection state to a session with SetUserPointer().
//
// A server needs somewhere to keep "who is this connection". The session is
// that place: SetUserPointer() hangs a shared_ptr<T> of your own off it, and
// user_ptr_typed<T>() gets it back, so any handler holding the session can
// reach the state without a session-id-to-player map of its own.
//
// This example gives every connection a ClientState, fills in the name when
// the client introduces itself, counts its messages, and drops it on
// disconnect.
//

#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/signal_handler.h"

#include "packets.h"

#include <algorithm>
#include <mutex>
#include <vector>

using namespace znet;

// What we hang off each session. Anything at all can go here; znet stores it
// as shared_ptr<void> and never looks inside.
struct ClientState {
  std::string name = "<anonymous>";
  uint32_t messages_received = 0;
};

// The session owns its ClientState, so this registry exists only so the server
// can walk every connected client. It is touched from several worker threads
// at once (see the Threading Model page), so it needs its own lock. The state
// *behind* each pointer does not: only that session's own thread reads it.
std::mutex g_clients_mutex;
std::vector<std::shared_ptr<ClientState>> g_clients;

std::shared_ptr<Codec> g_codec;

class MyPacketHandler
    : public PacketHandler<MyPacketHandler, HelloPacket, MessagePacket> {
 public:
  explicit MyPacketHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(std::shared_ptr<HelloPacket> packet) {
    // The state was attached before this handler could ever run, so it is
    // here. It is still worth checking if the given pointer is null because
    // it would lead to a crash. Better safe than sorry.
    std::shared_ptr<ClientState> state = session_->user_ptr_typed<ClientState>();
    if (!state) {
      ZNET_LOG_ERROR("Session {} has no ClientState attached!", session_->id());
      return;
    }

    state->name = packet->name;
    ZNET_LOG_INFO("Session {} is now known as '{}'.", session_->id(),
                  state->name);
  }

  void OnPacket(std::shared_ptr<MessagePacket> packet) {
    std::shared_ptr<ClientState> state = session_->user_ptr_typed<ClientState>();
    if (!state) {
      ZNET_LOG_ERROR("Session {} has no ClientState attached!", session_->id());
      return;
    }

    // No lock needed: one session never runs two callbacks at once, so this
    // state has exactly one thread touching it.
    state->messages_received++;

    ZNET_LOG_INFO("[{}] #{}: {}", state->name, state->messages_received,
                  packet->text);

    auto reply = std::make_shared<MessagePacket>();
    reply->text = state->name + ", that was message #" +
                  std::to_string(state->messages_received);
    session_->SendPacket(reply);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnNewSessionEvent(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(g_codec);

  // Attach the state before the handler, so no packet can arrive and find it
  // missing. This runs on the session's own thread, which is the only safe
  // place to call SetUserPointer on a live session.
  auto state = std::make_shared<ClientState>();
  session.SetUserPointer(state);

  session.SetHandler(std::make_shared<MyPacketHandler>(event.session()));

  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.push_back(state);
  }

  ZNET_LOG_INFO("Session {} connected, {} client(s) online.", session.id(),
                g_clients.size());
  return false;
}

bool OnDisconnectSessionEvent(ServerClientDisconnectedEvent& event) {
  PeerSession& session = *event.session();

  std::shared_ptr<ClientState> state = session.user_ptr_typed<ClientState>();
  if (!state) {
    return false;
  }

  ZNET_LOG_INFO("'{}' disconnected after {} message(s).", state->name,
                state->messages_received);

  // Drop the registry's reference. The session drops its own when it is
  // destroyed, and the ClientState goes with the last one, so there is nothing
  // to delete by hand.
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    auto it = std::find(g_clients.begin(), g_clients.end(), state);
    if (it != g_clients.end()) {
      std::iter_swap(it, g_clients.end() - 1);
      g_clients.pop_back();
    }
  }
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
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

  // one codec for every session: serializers are stateless and shared
  g_codec = MakeCodec();

  ServerConfig config{"localhost", 25000, std::chrono::seconds(10)};
  Server server{config};

  RegisterSignalHandler(
      [&server](Signal sig) -> bool {
        (void)sig;
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

  server.Wait();

  znet::Cleanup();
  return 0;
}
