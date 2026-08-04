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
// ZDT example client. Sends three kinds of traffic at once, each asking for the
// delivery it actually needs, and prints the transport's own counters so the
// difference is visible rather than asserted.
//
// To see it matter, impair the link first:
//   sudo tc qdisc add dev lo root netem delay 25ms loss 5%
//   sudo tc qdisc del dev lo root                 # to undo
//
// Chat arrives complete and in order. Positions show gaps, because a lost one
// is never retransmitted. Chunks all arrive; the server counts how many landed
// behind one already delivered, which is what asking for unordered permits but
// does not force.
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

namespace {

constexpr int kPositionsPerSecond = 20;

class ReplyHandler : public PacketHandler<ReplyHandler, ChatPacket> {
 public:
  void OnPacket(std::shared_ptr<ChatPacket> packet) {
    ZNET_LOG_INFO("[client] server says: {}", packet->text);
  }
};

// The transport's own view of the link. cwnd well below the cap means the
// controller is holding it there; retransmits with srtt near rtt_min means loss
// without queueing, which is a lossy link rather than a full one.
void LogMetrics(const std::shared_ptr<PeerSession>& session) {
  SessionMetrics m = session->metrics();
  if (m.connection_type != ConnectionType::ZDT) {
    return;
  }
  ZNET_LOG_INFO(
      "[client] srtt {}us  rtt_min {}us  cwnd {}  in_flight {}  retransmits {}  "
      "sent {}  refused {}",
      m.zdt.srtt_us, m.zdt.rtt_min_us, m.zdt.cwnd, m.zdt.in_flight,
      m.zdt.retransmits, m.common.messages_sent, m.common.send_failures);
}

void RunTraffic(std::shared_ptr<PeerSession> session) {
  // reliable + unordered: every chunk arrives, none waits for its predecessor
  for (int i = 0; i < kChunkCount; i++) {
    auto chunk = std::make_shared<ChunkPacket>();
    chunk->index = static_cast<uint32_t>(i);
    // sized so each chunk is its own datagram rather than being coalesced with
    // its neighbours, which is what makes a single loss visible as one late
    // chunk instead of two
    chunk->payload = std::string(1100, static_cast<char>('a' + (i % 26)));
    if (session->SendPacket(chunk, ChunkOptions()) != Result::Success) {
      // the queue is full, which is the only backpressure signal there is
      ZNET_LOG_WARN("[client] send queue full, dropping chunk {}", i);
    }
  }
  ZNET_LOG_INFO("[client] queued {} chunks on channel {}", kChunkCount,
                static_cast<int>(kChunkChannel));

  uint32_t tick = 0;
  int chat_counter = 0;
  auto next_chat = std::chrono::steady_clock::now();
  auto next_metrics = next_chat;

  while (session->IsAlive()) {
    // unreliable + unordered: a lost sample is replaced by the next one, so
    // retransmitting it would only add latency to fresher data
    auto position = std::make_shared<PositionPacket>();
    position->tick = ++tick;
    position->x = static_cast<float>(tick);
    position->y = static_cast<float>(tick) * 0.5f;
    position->z = 0.0f;
    session->SendPacket(position, PositionOptions());

    const auto now = std::chrono::steady_clock::now();
    if (now >= next_chat) {
      // reliable + ordered: this must arrive, and in the order sent
      auto chat = std::make_shared<ChatPacket>();
      chat->text = "message #" + std::to_string(++chat_counter);
      session->SendPacket(chat, ChatOptions());
      ZNET_LOG_INFO("[client] sent {}", chat->text);
      next_chat = now + std::chrono::seconds(1);
    }
    if (now >= next_metrics) {
      LogMetrics(session);
      next_metrics = now + std::chrono::seconds(5);
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(1000 / kPositionsPerSecond));
  }
}

bool OnConnected(ClientConnectedToServerEvent& event) {
  ZNET_LOG_INFO("[client] connected over ZDT");
  auto session = event.session();
  session->SetCodec(MakeCodec());
  session->SetHandler(std::make_shared<ReplyHandler>());

  // capturing the session keeps it alive for as long as the thread runs
  std::thread([session]() { RunTraffic(session); }).detach();
  return false;
}

bool OnDisconnected(ClientDisconnectedFromServerEvent&) {
  ZNET_LOG_INFO("[client] disconnected");
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnected));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnected));
}

}  // namespace

int main() {
  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // ZDT is the default; named here because this example is about it
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
