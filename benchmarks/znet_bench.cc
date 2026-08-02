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
// znet over TCP and over ZDT, on loopback, in one process.
//
// Read the numbers with the caveat in benchmarks/README.md: znet encrypts and
// compresses every packet by default, which ENet and RakNet do not, so this is
// measuring znet as configured rather than the transport in isolation.
//

#include "common/congestion.h"
#include "common/harness.h"
#include "common/impairment.h"
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
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace znet;

namespace {

enum BenchPacketType : PacketId { kPacketBench = 1 };

class BenchPacket : public Packet {
 public:
  BenchPacket() : Packet(kPacketBench) {}
  std::string payload;
  uint32_t seq = 0;
};

class BenchSerializer : public PacketSerializer<BenchPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<BenchPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->seq);
    buffer->WriteString(packet->payload);
    return buffer;
  }
  std::shared_ptr<BenchPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<BenchPacket>();
    packet->seq = buffer->ReadInt<uint32_t>();
    packet->payload = buffer->ReadString();
    return packet;
  }
};

std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketBench, std::make_unique<BenchSerializer>());
  return codec;
}

// Counts what the server receives.
class SinkHandler : public PacketHandler<SinkHandler, BenchPacket> {
 public:
  explicit SinkHandler(std::atomic_uint32_t* received) : received_(received) {}
  void OnPacket(std::shared_ptr<BenchPacket>) { received_->fetch_add(1); }

 private:
  std::atomic_uint32_t* received_;
};

// Echoes back, for the round-trip measurement.
class EchoHandler : public PacketHandler<EchoHandler, BenchPacket> {
 public:
  explicit EchoHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}
  void OnPacket(std::shared_ptr<BenchPacket> packet) {
    session_->SendPacket(packet);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

class ReplyHandler : public PacketHandler<ReplyHandler, BenchPacket> {
 public:
  explicit ReplyHandler(std::atomic_uint32_t* replies) : replies_(replies) {}
  void OnPacket(std::shared_ptr<BenchPacket>) { replies_->fetch_add(1); }

 private:
  std::atomic_uint32_t* replies_;
};

// Congestion case: bulk and probe share a session, told apart by size. Probes
// return on channel 1 so ZDT keeps them off the bulk sequence space; TCP
// ignores the channel and has one stream.
const SendOptions kProbeOptions = SendOptions().Channel(1);

class CongestionHandler : public PacketHandler<CongestionHandler, BenchPacket> {
 public:
  CongestionHandler(std::shared_ptr<PeerSession> session, size_t probe_bytes,
                    std::atomic_uint32_t* bulk)
      : session_(std::move(session)), probe_bytes_(probe_bytes), bulk_(bulk) {}
  void OnPacket(std::shared_ptr<BenchPacket> packet) {
    if (packet->payload.size() == probe_bytes_) {
      session_->SendPacket(packet, kProbeOptions);
      return;
    }
    bulk_->fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::shared_ptr<PeerSession> session_;
  size_t probe_bytes_;
  std::atomic_uint32_t* bulk_;
};

const char* TransportName(ConnectionType type) {
  return type == ConnectionType::ZDT ? "ZDT" : "TCP";
}

// Server-side profile. "raw" (no crypto, no compression) is the like-for-like
// row against ENet and RakNet; the default shows znet as shipped.
struct Profile {
  const char* suffix;
  bool encryption;
  CompressionType compression;
};

Profile g_profile{"", true, CompressionType::Default};

std::string LibraryName() {
  return std::string("znet") + g_profile.suffix;
}

// znet's TCP framing keeps a whole message in one buffer; ZDT fragments.
bool TCPCanCarry(size_t payload_bytes) {
  const size_t framing_slack = 64;  // packet id, length prefix, string prefix
  return payload_bytes + framing_slack < ZNET_MAX_BUFFER_SIZE;
}

bench::Impairment g_impair;

// What the server does with what it receives.
enum class ServerRole {
  Sink,        // count it; the throughput case
  Echo,        // bounce it back; the latency case
  Congestion,  // echo probes, count the rest
};

struct Harnessed {
  std::unique_ptr<Server> server;
  std::unique_ptr<Client> client;
  std::shared_ptr<PeerSession> client_session;
  std::atomic_bool ready{false};

  // Read by the event callbacks, which outlive Connect()'s stack frame, so run
  // state lives here rather than in Connect() parameters.
  ServerRole role = ServerRole::Sink;
  size_t probe_bytes = 0;
  std::atomic_uint32_t* server_received = nullptr;
  std::atomic_uint32_t* client_replies = nullptr;
};

// Brings up a loopback server/client pair and blocks until the session is live.
bool Connect(Harnessed& h, ConnectionType type, PortNumber port,
             bench::Clock::duration* connect_time) {
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(10), type};
  // Only the server configures these; the client adopts what it announces.
  server_config.child_options.common.encryption = g_profile.encryption;
  server_config.child_options.common.compression = g_profile.compression;
  bench::ApplyBenchQueueBounds(server_config.child_options);
  h.server = std::make_unique<Server>(server_config);
  h.server->SetEventCallback([&h](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&h](IncomingClientConnectedEvent& ev) {
          ev.session()->SetCodec(MakeCodec());
          switch (h.role) {
            case ServerRole::Echo:
              ev.session()->SetHandler(
                  std::make_shared<EchoHandler>(ev.session()));
              break;
            case ServerRole::Congestion:
              ev.session()->SetHandler(std::make_shared<CongestionHandler>(
                  ev.session(), h.probe_bytes, h.server_received));
              break;
            case ServerRole::Sink:
              ev.session()->SetHandler(
                  std::make_shared<SinkHandler>(h.server_received));
              break;
          }
          return false;
        });
  });
  if (h.server->Bind() != Result::Success ||
      h.server->Listen() != Result::Success) {
    return false;
  }

  auto start = bench::Clock::now();
  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(10), type};
  // the sending side, so its queue bounds matter most
  bench::ApplyBenchQueueBounds(client_config.options);
  h.client = std::make_unique<Client>(client_config);
  h.client->SetEventCallback([&h](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&h](ClientConnectedToServerEvent& ev) {
          ev.session()->SetCodec(MakeCodec());
          ev.session()->SetHandler(
              std::make_shared<ReplyHandler>(h.client_replies));
          h.client_session = ev.session();
          h.ready = true;
          return false;
        });
  });
  if (h.client->Bind() != Result::Success ||
      h.client->Connect() != Result::Success) {
    return false;
  }

  auto deadline = bench::Clock::now() + std::chrono::seconds(10);
  while (!h.ready && bench::Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  *connect_time = bench::Clock::now() - start;
  return h.ready.load();
}

void Teardown(Harnessed& h) {
  if (h.client) {
    h.client->Disconnect();
  }
  if (h.server) {
    h.server->Stop();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  h.client_session.reset();
  h.client.reset();
  h.server.reset();
}

// FreePort() races other processes; retry with a fresh port rather than lose a
// table row to a spurious failure.
bool ConnectWithRetry(Harnessed& h, ConnectionType type,
                      bench::Clock::duration* connect_time) {
  constexpr int kAttempts = 5;
  for (int attempt = 0; attempt < kAttempts; attempt++) {
    if (Connect(h, type, bench::FreePort(), connect_time)) {
      return true;
    }
    Teardown(h);
    h.ready = false;
  }
  return false;
}

// ZNET_BENCH_METRICS=1: protocol counters after each rep, for correlating a
// rate with the state that produced it. Off by default to keep tables diffable.
void MaybePrintMetrics(const Harnessed& h, ConnectionType type,
                       const char* case_name) {
  if (std::getenv("ZNET_BENCH_METRICS") == nullptr || !h.client_session) {
    return;
  }
  SessionMetrics m = h.client_session->metrics();
  std::printf("%-10s %-6s metrics    %-6s  mtu %5u  cwnd %5u  dgram_tx %8llu  "
              "rtx %6llu  nak_rx %5llu  in_drop %5llu  reasm_drop %5llu  "
              "srtt %6u us  rtt_min %6u us  rto %7u us\n",
              LibraryName().c_str(), TransportName(type), case_name,
              m.zdt.mtu, m.zdt.cwnd,
              static_cast<unsigned long long>(m.zdt.datagrams_sent),
              static_cast<unsigned long long>(m.zdt.retransmits),
              static_cast<unsigned long long>(m.zdt.naks_received),
              static_cast<unsigned long long>(m.zdt.inbound_dropped),
              static_cast<unsigned long long>(m.zdt.reassemblies_dropped),
              m.zdt.srtt_us, m.zdt.rtt_min_us, m.zdt.rto_us);
}

void RunThroughput(ConnectionType type, const bench::Workload& w) {
  if (type == ConnectionType::TCP && !TCPCanCarry(w.payload_bytes)) {
    std::printf("%-10s %-6s throughput %-6s  unsupported (exceeds ZNET_MAX_BUFFER_SIZE framing)\n",
                LibraryName().c_str(), TransportName(type), w.name);
    return;
  }
  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<bench::LoopResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    std::atomic_uint32_t received{0};
    std::atomic_uint32_t replies{0};
    bench::Clock::duration connect_time{};
    Harnessed h;
    h.role = ServerRole::Sink;
    h.server_received = &received;
    h.client_replies = &replies;
    if (!ConnectWithRetry(h, type, &connect_time)) {
      std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n",
                  LibraryName().c_str(), TransportName(type), w.name);
      Teardown(h);
      continue;
    }

    uint32_t seq = 0;
    uint32_t counted = 0;
    reps.push_back(bench::RunThroughputLoop(
        w,
        [&]() {
          if (!h.client_session->IsAlive()) {
            return false;
          }
          auto packet = std::make_shared<BenchPacket>();
          packet->seq = seq;
          packet->payload = payload;
          // refusal is the backpressure signal the loop drains on
          if (!h.client_session->SendPacket(packet)) {
            return false;
          }
          seq++;
          return true;
        },
        [&]() {
          // znet services its own sockets; only counters to read here
          uint32_t now = received.load();
          uint32_t progress = now - counted;
          counted = now;
          return progress;
        },
        bench::ThroughputWarmup(g_impair)));
    MaybePrintMetrics(h, type, w.name);
    Teardown(h);
  }
  bench::ReportThroughput(LibraryName().c_str(), TransportName(type), w, reps);
}

void RunLatency(ConnectionType type, const bench::Workload& w) {
  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<std::vector<double>> rep_samples;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    std::atomic_uint32_t received{0};
    std::atomic_uint32_t replies{0};
    bench::Clock::duration connect_time{};
    Harnessed h;
    h.role = ServerRole::Echo;
    h.server_received = &received;
    h.client_replies = &replies;
    if (!ConnectWithRetry(h, type, &connect_time)) {
      std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n",
                  LibraryName().c_str(), TransportName(type), w.name);
      Teardown(h);
      continue;
    }
    if (rep == 0) {
      bench::ReportConnect(LibraryName().c_str(), TransportName(type),
                           connect_time);
    }

    uint32_t seq = 0;
    uint32_t before = 0;
    rep_samples.push_back(bench::RunLatencyLoop(
        w,
        [&]() {
          if (!h.client_session->IsAlive()) {
            return false;
          }
          before = replies.load();
          auto packet = std::make_shared<BenchPacket>();
          packet->seq = seq++;
          packet->payload = payload;
          return h.client_session->SendPacket(packet);
        },
        [&]() {
          std::this_thread::yield();
          return replies.load() != before;
        }));
    Teardown(h);
  }
  bench::ReportLatency(LibraryName().c_str(), TransportName(type), w,
                       rep_samples);
}

void RunCongestion(ConnectionType type, const bench::CongestionCase& c) {
  if (type == ConnectionType::TCP && !TCPCanCarry(c.bulk_bytes)) {
    std::printf("%-10s %-6s congestion %-6s  unsupported (exceeds ZNET_MAX_BUFFER_SIZE framing)\n",
                LibraryName().c_str(), TransportName(type), c.name);
    return;
  }
  const std::string bulk_payload = bench::MakePayload(c.bulk_bytes);
  const std::string probe_payload = bench::MakePayload(c.probe_bytes);
  std::vector<bench::CongestionResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    std::atomic_uint32_t bulk{0};
    std::atomic_uint32_t probes{0};
    bench::Clock::duration connect_time{};
    Harnessed h;
    h.role = ServerRole::Congestion;
    h.probe_bytes = c.probe_bytes;
    h.server_received = &bulk;
    h.client_replies = &probes;
    if (!ConnectWithRetry(h, type, &connect_time)) {
      std::printf("%-10s %-6s congestion %-6s  FAILED to connect\n",
                  LibraryName().c_str(), TransportName(type), c.name);
      Teardown(h);
      continue;
    }

    uint32_t seq = 0;
    uint32_t bulk_counted = 0;
    uint32_t probe_counted = 0;
    reps.push_back(bench::RunCongestionLoop(
        c,
        [&]() {
          if (!h.client_session->IsAlive()) {
            return false;
          }
          auto packet = std::make_shared<BenchPacket>();
          packet->seq = seq;
          packet->payload = bulk_payload;
          if (!h.client_session->SendPacket(packet)) {
            return false;
          }
          seq++;
          return true;
        },
        [&]() {
          if (!h.client_session->IsAlive()) {
            return false;
          }
          auto packet = std::make_shared<BenchPacket>();
          packet->seq = seq;
          packet->payload = probe_payload;
          return h.client_session->SendPacket(packet, kProbeOptions);
        },
        [&]() {
          bench::PumpCounts counts;
          uint32_t now_bulk = bulk.load();
          counts.bulk = now_bulk - bulk_counted;
          bulk_counted = now_bulk;
          uint32_t now_probes = probes.load();
          counts.probes = now_probes - probe_counted;
          probe_counted = now_probes;
          return counts;
        }));
    MaybePrintMetrics(h, type, c.name);
    Teardown(h);
  }
  bench::ReportCongestionCase(LibraryName().c_str(), TransportName(type), c,
                              reps,
                              type == ConnectionType::ZDT ? "channel" : "none");
}

}  // namespace

int main() {
  if (Init() != Result::Success) {
    std::fprintf(stderr, "failed to initialize znet\n");
    return 1;
  }
  std::printf("znet %s\n", VersionString());
  g_impair = bench::Impairment::FromEnv();
  bench::NoteImpairment(g_impair);
  bench::AnnounceRunSettings();
  bench::Note("default = AES + zstd as shipped; raw = both off, which is what");
  bench::Note("ENet and RakNet do. See README.md.");

  // Narrowing knobs, for profiling:
  //   ZNET_BENCH_TRANSPORT=zdt ZNET_BENCH_CASE=8KB ZNET_BENCH_SKIP_LATENCY=1
  const char* only_transport = std::getenv("ZNET_BENCH_TRANSPORT");
  const char* only_case = std::getenv("ZNET_BENCH_CASE");
  const bool skip_latency = std::getenv("ZNET_BENCH_SKIP_LATENCY") != nullptr;
  // congestion adds ~20s per transport per profile
  const bool skip_congestion =
      std::getenv("ZNET_BENCH_SKIP_CONGESTION") != nullptr;

  const Profile profiles[] = {
      {"", true, CompressionType::Default},
      {"-raw", false, CompressionType::None},
  };
  for (const Profile& profile : profiles) {
    g_profile = profile;
    for (ConnectionType type : {ConnectionType::TCP, ConnectionType::ZDT}) {
      if (only_transport != nullptr) {
        std::string want(only_transport);
        for (char& c : want) {
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (want != TransportName(type)) {
          continue;
        }
      }
      bench::PrintHeader(LibraryName().c_str(), TransportName(type));
      for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
        if (only_case != nullptr && std::string(only_case) != w.name) {
          continue;
        }
        RunThroughput(type, w);
      }
      if (!skip_latency) {
        RunLatency(type, bench::ImpairedLatencyWorkload(g_impair));
      }
      if (!skip_congestion) {
        for (const auto& c : bench::DefaultCongestionCases()) {
          if (only_case != nullptr && std::string(only_case) != c.name) {
            continue;
          }
          RunCongestion(type, c);
        }
      }
    }
  }

  Cleanup();
  return 0;
}
