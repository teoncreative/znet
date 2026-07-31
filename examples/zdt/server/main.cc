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
// ZDT example server. Reports what each delivery mode actually did rather than
// just echoing, since the difference between them is the point of the example.
//
// Run this, then the client. Over an impaired link (see the client's header)
// the three diverge: chat stays complete and in order, positions show gaps, and
// chunks all arrive without any of them waiting on an earlier one.
//

#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/signal_handler.h"

#include "packets.h"

#include <set>

using namespace znet;

namespace {

// One per connection. A session's handlers are serialized, so plain members
// need no locking here.
class DemoHandler
    : public PacketHandler<DemoHandler, ChatPacket, PositionPacket, ChunkPacket> {
 public:
  explicit DemoHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  // reliable + ordered: arrives complete, in the order sent
  void OnPacket(std::shared_ptr<ChatPacket> packet) {
    chat_received_++;
    ZNET_LOG_INFO("[server] chat #{}: {}", chat_received_, packet->text);

    auto reply = std::make_shared<ChatPacket>();
    reply->text = "ack " + packet->text;
    session_->SendPacket(reply, ChatOptions());
  }

  // unreliable + unordered: a gap between the tick count and the arrival count
  // is losses that were deliberately not retransmitted, which is the trade
  void OnPacket(std::shared_ptr<PositionPacket> packet) {
    positions_received_++;
    if (packet->tick > highest_tick_) {
      highest_tick_ = packet->tick;
    }
    if (positions_received_ % 40 == 0) {
      ZNET_LOG_INFO(
          "[server] positions: {} arrived of {} sent, {} never made it",
          positions_received_, highest_tick_,
          highest_tick_ - positions_received_);
    }
  }

  // reliable + unordered: every index eventually arrives, but not in order,
  // because none of them waits for an earlier one
  void OnPacket(std::shared_ptr<ChunkPacket> packet) {
    const bool behind = packet->index < highest_chunk_;
    if (packet->index > highest_chunk_) {
      highest_chunk_ = packet->index;
    }
    chunks_.insert(packet->index);
    if (behind) {
      chunks_out_of_order_++;
    }
    ZNET_LOG_INFO("[server] chunk {} ({} bytes){}", packet->index,
                  packet->payload.size(),
                  behind ? "  <- behind one already delivered" : "");
    if (chunks_.size() == kChunkCount) {
      ZNET_LOG_INFO("[server] all {} chunks arrived, {} of them out of order",
                    kChunkCount, chunks_out_of_order_);
    }
  }

 private:
  std::shared_ptr<PeerSession> session_;
  uint32_t chat_received_ = 0;
  uint32_t positions_received_ = 0;
  uint32_t highest_tick_ = 0;
  uint32_t highest_chunk_ = 0;
  uint32_t chunks_out_of_order_ = 0;
  std::set<uint32_t> chunks_;
};

// serializers are stateless, so one codec serves every session
std::shared_ptr<Codec> g_codec;

bool OnNewSession(IncomingClientConnectedEvent& event) {
  ZNET_LOG_INFO("[server] client connected over ZDT: {}",
                event.session()->remote_address()->readable());
  event.session()->SetCodec(g_codec);
  event.session()->SetHandler(std::make_shared<DemoHandler>(event.session()));
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

}  // namespace

int main() {
  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }
  g_codec = MakeCodec();

  // ZDT is the default; named here because this example is about it
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
