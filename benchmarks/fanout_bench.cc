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
// Fan-out: one application thread broadcasting to many sessions.
//
// znet_bench.cc measures one session driven by one sender. This measures the
// shape a game server actually has, a single simulation thread pushing state to
// every connected client, which rewards the opposite arrangement: encoding on
// the calling thread pipelines well with one session but serializes N ways
// here, while encoding on the session's worker spreads it across the pool.
//

#include "common/harness.h"

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/version.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace znet;

namespace {

enum FanoutPacketType : PacketId { kPacketFanout = 1 };

class FanoutPacket : public Packet {
 public:
  FanoutPacket() : Packet(kPacketFanout) {}
  std::string payload;
  uint32_t seq = 0;
};

class FanoutSerializer : public PacketSerializer<FanoutPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<FanoutPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->seq);
    buffer->WriteString(packet->payload);
    return buffer;
  }
  std::shared_ptr<FanoutPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<FanoutPacket>();
    packet->seq = buffer->ReadInt<uint32_t>();
    packet->payload = buffer->ReadString();
    return packet;
  }
};

std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketFanout, std::make_unique<FanoutSerializer>());
  return codec;
}

// Every client funnels into one counter; the broadcast is done when it reaches
// clients * per_client.
class CountingHandler : public PacketHandler<CountingHandler, FanoutPacket> {
 public:
  explicit CountingHandler(std::atomic_uint32_t* received) : received_(received) {}
  void OnPacket(std::shared_ptr<FanoutPacket>) {
    received_->fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::atomic_uint32_t* received_;
};

PortNumber FreePort() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  PortNumber port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

struct FanoutResult {
  uint32_t delivered = 0;
  double seconds = 0.0;
};

FanoutResult RunFanout(const char* profile, ConnectionType type,
                       uint32_t client_count, uint32_t per_client,
                       size_t payload_bytes, bool secure) {
  const std::string payload = bench::MakePayload(payload_bytes);
  std::atomic_uint32_t received{0};
  std::atomic_uint32_t clients_ready{0};

  std::mutex sessions_mutex;
  std::vector<std::shared_ptr<PeerSession>> sessions;

  PortNumber port = FreePort();
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(10), type};
  server_config.child_options.common.encryption = secure;
  server_config.child_options.common.compression =
      secure ? CompressionType::Default : CompressionType::None;

  Server server{server_config};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent& ev) {
          ev.session()->SetCodec(MakeCodec());
          std::lock_guard<std::mutex> lock(sessions_mutex);
          sessions.push_back(ev.session());
          return false;
        });
  });
  server.Bind();
  server.Listen();

  std::vector<std::unique_ptr<Client>> clients;
  clients.reserve(client_count);
  for (uint32_t i = 0; i < client_count; i++) {
    ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(10), type};
    auto client = std::unique_ptr<Client>(new Client{client_config});
    client->SetEventCallback([&](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<ClientConnectedToServerEvent>(
          [&](ClientConnectedToServerEvent& ev) {
            ev.session()->SetCodec(MakeCodec());
            ev.session()->SetHandler(std::make_shared<CountingHandler>(&received));
            clients_ready.fetch_add(1);
            return false;
          });
    });
    client->Bind();
    client->Connect();
    clients.push_back(std::move(client));
  }

  // both sides: the server must hold every session, and every client must have
  // installed its handler, or the first broadcasts land on a session with no
  // codec and are silently dropped.
  auto connect_deadline = bench::Clock::now() + std::chrono::seconds(30);
  while (bench::Clock::now() < connect_deadline) {
    size_t have = 0;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      have = sessions.size();
    }
    if (have >= client_count && clients_ready.load() >= client_count) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::vector<std::shared_ptr<PeerSession>> targets;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    targets = sessions;
  }
  if (targets.size() < client_count) {
    std::printf("%-10s %-6s fanout     %ux%u  only %zu/%u sessions connected\n",
                profile, type == ConnectionType::TCP ? "TCP" : "ZDT",
                client_count, per_client, targets.size(), client_count);
    return {};
  }

  const uint32_t total = client_count * per_client;
  auto deadline = bench::Clock::now() + std::chrono::seconds(120);
  auto started = bench::Clock::now();

  // the fan-out itself: one thread, every session, round after round. A refusal
  // is backpressure, so spin on it rather than dropping the message, or the
  // delivered count stops meaning anything.
  for (uint32_t round = 0; round < per_client; round++) {
    for (std::shared_ptr<PeerSession>& session : targets) {
      auto packet = std::make_shared<FanoutPacket>();
      packet->seq = round;
      packet->payload = payload;
      while (!session->SendPacket(packet)) {
        if (bench::Clock::now() > deadline || !session->IsAlive()) {
          break;
        }
        std::this_thread::yield();
      }
    }
  }

  while (received.load() < total && bench::Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto elapsed = std::chrono::duration<double>(bench::Clock::now() - started).count();

  uint32_t delivered = received.load();
  double rate = elapsed > 0 ? delivered / elapsed : 0;
  double mib = elapsed > 0
                   ? (static_cast<double>(delivered) * static_cast<double>(payload_bytes))
                         / (1024.0 * 1024.0) / elapsed
                   : 0;
  std::printf("%-10s %-6s fanout     %4ux%-6u %8u msgs  %8.3f s  %10.0f msg/s  %8.1f MiB/s\n",
              profile, type == ConnectionType::TCP ? "TCP" : "ZDT", client_count,
              per_client, delivered, elapsed, rate, mib);
  std::fflush(stdout);

  for (auto& client : clients) {
    client->Disconnect();
  }
  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return {delivered, elapsed};
}

}  // namespace

int main() {
  Init();
  std::printf("znet %s fan-out\n", ZNET_VERSION_STRING);
  std::fflush(stdout);

  struct Case {
    uint32_t clients;
    uint32_t per_client;
    size_t payload;
  };
  const Case cases[] = {
      {8, 4000, 1024},
      {32, 2000, 1024},
      {64, 1000, 1024},
  };

  for (const Case& c : cases) {
    RunFanout("znet", ConnectionType::ZDT, c.clients, c.per_client, c.payload, true);
  }
  for (const Case& c : cases) {
    RunFanout("znet-raw", ConnectionType::ZDT, c.clients, c.per_client, c.payload, false);
  }
  for (const Case& c : cases) {
    RunFanout("znet", ConnectionType::TCP, c.clients, c.per_client, c.payload, true);
  }
  return 0;
}
