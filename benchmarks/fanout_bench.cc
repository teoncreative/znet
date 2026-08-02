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
// Fan-out: one application thread broadcasting to many sessions, the shape a
// game server has. Rewards the opposite arrangement from znet_bench's
// one-session pipeline.
//

#include "common/harness.h"
#include "common/znet_tuning.h"

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

// Every client funnels into one counter.
class CountingHandler : public PacketHandler<CountingHandler, FanoutPacket> {
 public:
  explicit CountingHandler(std::atomic_uint32_t* received) : received_(received) {}
  void OnPacket(std::shared_ptr<FanoutPacket>) {
    received_->fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::atomic_uint32_t* received_;
};

struct FanoutResult {
  bool ok = false;
  uint32_t delivered = 0;
  double seconds = 0.0;
  bool timed_out = false;
  double cpu_seconds = 0.0;
};

FanoutResult RunFanout(const char* profile, ConnectionType type,
                       uint32_t client_count, uint32_t per_client,
                       size_t payload_bytes, bool secure) {
  const std::string payload = bench::MakePayload(payload_bytes);
  const char* transport = type == ConnectionType::TCP ? "TCP" : "ZDT";
  std::atomic_uint32_t received{0};
  std::atomic_uint32_t clients_ready{0};

  std::mutex sessions_mutex;
  std::vector<std::shared_ptr<PeerSession>> sessions;

  PortNumber port = bench::FreePort();
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(10), type};
  server_config.child_options.common.encryption = secure;
  server_config.child_options.common.compression =
      secure ? CompressionType::Default : CompressionType::None;
  // same bounds as znet_bench, so the two tables measure the same regime
  bench::ApplyBenchQueueBounds(server_config.child_options);

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
  if (server.Bind() != Result::Success ||
      server.Listen() != Result::Success) {
    std::printf("%-10s %-6s fanout     %ux%u  FAILED to bind/listen\n", profile,
                transport, client_count, per_client);
    return {};
  }

  std::vector<std::unique_ptr<Client>> clients;
  clients.reserve(client_count);
  for (uint32_t i = 0; i < client_count; i++) {
    ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(10), type};
    bench::ApplyBenchQueueBounds(client_config.options);
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

  auto teardown = [&]() {
    for (auto& client : clients) {
      client->Disconnect();
    }
    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  };

  // both sides ready, or the first broadcasts land on sessions with no codec
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
                profile, transport, client_count, per_client, targets.size(),
                client_count);
    teardown();
    return {};
  }

  const uint32_t total = client_count * per_client;
  const double cpu_start = bench::ProcessCPUSeconds();
  auto deadline = bench::Clock::now() + std::chrono::seconds(120);
  auto started = bench::Clock::now();

  // a refusal is backpressure: spin, don't drop
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

  FanoutResult out;
  out.ok = true;
  out.delivered = received.load();
  out.seconds =
      std::chrono::duration<double>(bench::Clock::now() - started).count();
  out.cpu_seconds = bench::ProcessCPUSeconds() - cpu_start;
  out.timed_out = out.delivered < total;
  teardown();
  return out;
}

// Median rep by msg/s; CSV gets every rep.
void ReportFanout(const char* profile, ConnectionType type,
                  uint32_t client_count, uint32_t per_client,
                  size_t payload_bytes, const std::vector<FanoutResult>& reps) {
  if (reps.empty()) {
    return;
  }
  const char* transport = type == ConnectionType::TCP ? "TCP" : "ZDT";
  char case_name[32];
  std::snprintf(case_name, sizeof(case_name), "%ux%u", client_count, per_client);

  for (size_t i = 0; i < reps.size(); i++) {
    bench::CsvRow row;
    row.kind = "fanout";
    row.library = profile;
    row.transport = transport;
    row.case_name = case_name;
    row.rep = static_cast<int>(i + 1);
    row.delivered = reps[i].delivered;
    row.seconds = reps[i].seconds;
    row.msg_per_s = reps[i].seconds > 0 ? reps[i].delivered / reps[i].seconds : 0;
    row.mib_per_s = reps[i].seconds > 0
                        ? (static_cast<double>(reps[i].delivered) *
                           static_cast<double>(payload_bytes)) /
                              reps[i].seconds / (1024.0 * 1024.0)
                        : 0;
    row.timed_out = reps[i].timed_out ? 1 : 0;
    row.cpu_us_per_msg = reps[i].delivered > 0
                             ? reps[i].cpu_seconds * 1e6 / reps[i].delivered
                             : NAN;
    EmitCsv(row);
  }

  std::vector<FanoutResult> sorted = reps;
  std::sort(sorted.begin(), sorted.end(),
            [](const FanoutResult& a, const FanoutResult& b) {
              double ra = a.seconds > 0 ? a.delivered / a.seconds : 0;
              double rb = b.seconds > 0 ? b.delivered / b.seconds : 0;
              return ra < rb;
            });
  const FanoutResult& mid = sorted[sorted.size() / 2];
  double rate = mid.seconds > 0 ? mid.delivered / mid.seconds : 0;
  double mib = mid.seconds > 0 ? (static_cast<double>(mid.delivered) *
                                  static_cast<double>(payload_bytes)) /
                                     (1024.0 * 1024.0) / mid.seconds
                               : 0;
  double cpu_us =
      mid.delivered > 0 ? mid.cpu_seconds * 1e6 / mid.delivered : 0;
  std::printf("%-10s %-6s fanout     %4ux%-6u %8u msgs  %8.3f s  %10.0f msg/s  %8.1f MiB/s  %7.2f cpu-us/msg",
              profile, transport, client_count, per_client, mid.delivered,
              mid.seconds, rate, mib, cpu_us);
  if (mid.timed_out) {
    std::printf("  TIMEOUT (%u/%u in 120 s)", mid.delivered,
                client_count * per_client);
  }
  if (reps.size() > 1) {
    double lo = sorted.front().seconds > 0
                    ? sorted.front().delivered / sorted.front().seconds
                    : 0;
    double hi = sorted.back().seconds > 0
                    ? sorted.back().delivered / sorted.back().seconds
                    : 0;
    std::printf("  [%zu reps: %.0f..%.0f msg/s]", reps.size(), lo, hi);
  }
  std::printf("\n");
  std::fflush(stdout);
}

void RunCase(const char* profile, ConnectionType type, uint32_t clients,
             uint32_t per_client, size_t payload, bool secure) {
  std::vector<FanoutResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    FanoutResult r = RunFanout(profile, type, clients, per_client, payload, secure);
    if (r.ok) {
      reps.push_back(r);
    }
  }
  ReportFanout(profile, type, clients, per_client, payload, reps);
}

}  // namespace

int main() {
  if (Init() != Result::Success) {
    std::fprintf(stderr, "failed to initialize znet\n");
    return 1;
  }
  std::printf("znet %s fan-out\n", ZNET_VERSION_STRING);
  bench::AnnounceRunSettings();
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
    RunCase("znet", ConnectionType::ZDT, c.clients, c.per_client, c.payload, true);
  }
  for (const Case& c : cases) {
    RunCase("znet-raw", ConnectionType::ZDT, c.clients, c.per_client, c.payload, false);
  }
  for (const Case& c : cases) {
    RunCase("znet", ConnectionType::TCP, c.clients, c.per_client, c.payload, true);
  }

  Cleanup();
  return 0;
}
