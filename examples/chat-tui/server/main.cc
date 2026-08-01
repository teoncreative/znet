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
// Chat room server: owns the room list, who is in which room, and the fan-out.
//
// Per-connection state hangs off the session with SetUserPointer(), so no
// session-id-to-user map is needed. The one shared structure is the roster of
// live sessions, which several workers touch at once and which therefore has a
// lock; see the Threading Model page.
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
#include <cstdlib>
#include <random>
#include <vector>

using namespace znet;

static const std::vector<std::string> kRooms = {"#general", "#dev", "#random"};

// Hung off each session. Only that session's own thread touches it, so it
// needs no lock of its own: one session never runs two callbacks at once.
struct ClientState {
  std::string name;
  std::string room = kRooms[0];
};

// Names are assigned, never chosen, so two clients cannot collide and nobody
// picks a name meant to look like someone else's. adjective-animal-NN gives
// 40 * 40 * 90 = 144,000 combinations, which is plenty for a chat room and
// still short enough to read out loud.
static const char* const kAdjectives[] = {
    "amber",  "brave",  "bright", "calm",   "clever", "cosmic", "crimson",
    "curious", "dapper", "eager",  "electric", "fearless", "gentle", "golden",
    "happy",  "hidden", "jolly",  "keen",   "lively", "lucky",  "mellow",
    "merry",  "nimble", "noble",  "polar",  "proud",  "quiet",  "rapid",
    "rustic", "silent", "silver", "sleepy", "solar",  "spry",   "steady",
    "sunny",  "swift",  "tidy",   "velvet", "wandering",
};

static const char* const kAnimals[] = {
    "otter",   "badger",  "falcon", "heron",   "ibex",    "jackal", "kestrel",
    "lemur",   "lynx",    "marten", "narwhal", "ocelot",  "osprey", "panda",
    "puffin",  "quokka",  "raven",  "salmon",  "tapir",   "vulture", "walrus",
    "wombat",  "yak",     "zebra",  "beaver",  "cormorant", "dingo", "egret",
    "ferret",  "gecko",   "hare",   "impala",  "koala",   "manatee", "newt",
    "pelican", "ptarmigan", "shrew", "stoat",  "weasel",
};

constexpr size_t kAdjectiveCount = sizeof(kAdjectives) / sizeof(kAdjectives[0]);
constexpr size_t kAnimalCount = sizeof(kAnimals) / sizeof(kAnimals[0]);

// The roster exists so the server can fan a message out to a room. Different
// sessions run on different workers, so every access is under the mutex.
static std::mutex g_roster_mutex;
static std::vector<std::shared_ptr<PeerSession>> g_roster;

static std::shared_ptr<Codec> g_codec;

// Both of these require g_roster_mutex to already be held, hence the suffix:
// generating a name and inserting the session that takes it has to be one
// atomic step, or two connections racing could be handed the same name.
static bool NameTakenLocked(const std::string& name) {
  for (const std::shared_ptr<PeerSession>& session : g_roster) {
    std::shared_ptr<ClientState> state = session->user_ptr_typed<ClientState>();
    if (state && state->name == name) {
      return true;
    }
  }
  return false;
}

static std::string GenerateNameLocked() {
  // Seeded once. Guarded by the roster mutex like everything else here, so no
  // thread_local and no second lock.
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<size_t> adjective(0, kAdjectiveCount - 1);
  std::uniform_int_distribution<size_t> animal(0, kAnimalCount - 1);
  std::uniform_int_distribution<int> number(10, 99);

  for (int attempt = 0; attempt < 64; attempt++) {
    std::string candidate = std::string(kAdjectives[adjective(rng)]) + "-" +
                            kAnimals[animal(rng)] + "-" +
                            std::to_string(number(rng));
    if (!NameTakenLocked(candidate)) {
      return candidate;
    }
  }

  // Retrying forever would hang the acceptor once the name space filled up, so
  // fall back to something monotonic that cannot collide.
  static uint64_t fallback = 0;
  return "guest-" + std::to_string(++fallback);
}

// Sends to everyone currently in `room`. SendPacket is safe from any thread and
// only queues, so holding the roster lock across it does not block on I/O.
static void Broadcast(const std::string& room, std::shared_ptr<Packet> packet) {
  std::lock_guard<std::mutex> lock(g_roster_mutex);
  for (const std::shared_ptr<PeerSession>& session : g_roster) {
    std::shared_ptr<ClientState> state = session->user_ptr_typed<ClientState>();
    if (state && state->room == room) {
      session->SendPacket(packet);
    }
  }
}

static void BroadcastSystem(const std::string& room, const std::string& text) {
  auto notice = std::make_shared<SystemPacket>();
  notice->room = room;
  notice->text = text;
  Broadcast(room, notice);
}

class ChatHandler
    : public PacketHandler<ChatHandler, SelectRoomPacket, ChatPacket> {
 public:
  explicit ChatHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(std::shared_ptr<SelectRoomPacket> packet) {
    std::shared_ptr<ClientState> state = session_->user_ptr_typed<ClientState>();
    if (!state) {
      return;
    }
    // Only rooms the server offers. A client is free to ask for anything.
    if (std::find(kRooms.begin(), kRooms.end(), packet->room) == kRooms.end()) {
      return;
    }
    if (packet->room == state->room) {
      return;
    }

    const std::string previous = state->room;
    BroadcastSystem(previous, state->name + " left");
    state->room = packet->room;
    BroadcastSystem(state->room, state->name + " joined");
  }

  void OnPacket(std::shared_ptr<ChatPacket> packet) {
    std::shared_ptr<ClientState> state = session_->user_ptr_typed<ClientState>();
    if (!state) {
      return;
    }

    // The author and the room come from the server's own state, never from the
    // packet, so a client cannot speak as someone else or into a room it is
    // not in.
    auto message = std::make_shared<ChatPacket>();
    message->room = state->room;
    message->author = state->name;
    message->text = Sanitize(packet->text);
    if (message->text.empty()) {
      return;
    }

    ZNET_LOG_INFO("[{}] {}: {}", message->room, message->author, message->text);
    Broadcast(state->room, message);
  }

 private:
  // Strips control characters so a peer cannot move the cursor around in
  // another client's terminal, and bounds the length.
  static std::string Sanitize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
      if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
        out.push_back(c);
      }
      if (out.size() >= kMaxLength) {
        break;
      }
    }
    return out;
  }

  static const size_t kMaxLength = 240;

  std::shared_ptr<PeerSession> session_;
};

bool OnClientConnected(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();

  session.SetCodec(g_codec);

  auto state = std::make_shared<ClientState>();

  // Naming and joining the roster happen under one lock, so a name is never
  // handed out twice: a concurrent connect either sees this session already in
  // the roster or has not started looking yet.
  {
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    state->name = GenerateNameLocked();
    // Attached before the handler, so no packet can arrive and find it missing.
    session.SetUserPointer(state);
    g_roster.push_back(event.session());
  }

  session.SetHandler(std::make_shared<ChatHandler>(event.session()));

  // The session is ready to send the moment this event fires, so the client
  // learns its name without a round trip.
  auto welcome = std::make_shared<WelcomePacket>();
  welcome->name = state->name;
  welcome->rooms = kRooms;
  session.SendPacket(welcome);

  ZNET_LOG_INFO("'{}' connected and joined {}.", state->name, state->room);
  // Outside the lock: Broadcast takes it, and it is not recursive.
  BroadcastSystem(state->room, state->name + " joined");
  return false;
}

bool OnClientDisconnected(ServerClientDisconnectedEvent& event) {
  std::shared_ptr<ClientState> state =
      event.session()->user_ptr_typed<ClientState>();

  {
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    auto it = std::find(g_roster.begin(), g_roster.end(), event.session());
    if (it != g_roster.end()) {
      std::iter_swap(it, g_roster.end() - 1);
      g_roster.pop_back();
    }
  }

  // Announce after dropping them, so they are not told they left.
  if (state) {
    ZNET_LOG_INFO("'{}' disconnected.", state->name);
    BroadcastSystem(state->room, state->name + " left");
  }
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnClientConnected));
  dispatcher.Dispatch<ServerClientDisconnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnClientDisconnected));
}

int main(int argc, char** argv) {
  Result result;

  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  g_codec = MakeCodec();

  // Optional port, so a second server can run alongside one already using the
  // default rather than failing to bind.
  PortNumber port = 25000;
  if (argc > 1) {
    port = static_cast<PortNumber>(std::atoi(argv[1]));
  }

  ServerConfig config{"localhost", port, std::chrono::seconds(30)};
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

  ZNET_LOG_INFO("Chat server listening on localhost:25000.");
  server.Wait();

  znet::Cleanup();
  return 0;
}
