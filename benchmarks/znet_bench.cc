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

#include "common/harness.h"
#include "common/impairment.h"

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

using namespace znet;

namespace {

enum BenchPacketType { PACKET_BENCH = 1 };

class BenchPacket : public Packet {
 public:
  BenchPacket() : Packet(PACKET_BENCH) {}
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
  codec->Add(PACKET_BENCH, std::make_unique<BenchSerializer>());
  return codec;
}

PortNumber FreePort() {
  // Bind an ephemeral port, note what the OS picked, and release it. The port
  // can be taken again before we rebind it, so callers go through
  // ConnectWithRetry rather than trusting one attempt.
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

const char* TransportName(ConnectionType type) {
  return type == ConnectionType::ZDT ? "ZDT" : "TCP";
}

// What the server applies to accepted sessions. ENet and RakNet send plaintext
// and uncompressed, so the "raw" profile is the like-for-like comparison
// against them; the default profile shows what znet costs as shipped.
struct Profile {
  const char* suffix;
  bool encryption;
  CompressionType compression;
};

Profile g_profile{"", true, CompressionType::Default};

// Appended to the library column so profiles are distinguishable in one table.
std::string LibraryName() {
  return std::string("znet") + g_profile.suffix;
}

// znet's TCP framing keeps a whole message inside one buffer, so anything at or
// above this cannot be sent over TCP at all. ZDT fragments instead.
bool TCPCanCarry(size_t payload_bytes) {
  const size_t framing_slack = 64;  // packet id, length prefix, string prefix
  return payload_bytes + framing_slack < ZNET_MAX_BUFFER_SIZE;
}

// impairment is read once; an empty config leaves the relay out of the path
// entirely, so a clean run is byte-for-byte the old direct-to-server setup.
bench::Impairment g_impair;

struct Harnessed {
  std::unique_ptr<Server> server;
  std::unique_ptr<Client> client;
  std::shared_ptr<PeerSession> client_session;
  std::atomic_bool ready{false};
};

// Brings up a loopback server/client pair and blocks until the session is live.
// Lifts the queue bounds clear of the workload so the table measures protocol
// cost rather than buffer sizing. The shared loop keeps at most 4096 messages
// outstanding, and an 8 KiB message spans about six datagrams, so ~25k inbound
// datagrams is the real ceiling; these sit well above it.
void ApplyBenchQueueBounds(SessionOptions& options) {
  options.common.send_queue_capacity = 65536;
  options.zdt.outbound_queue_capacity = 65536;
  options.zdt.max_inbox_datagrams = 65536;
  options.zdt.max_reassemblies = 8192;
}

// `make_server_handler` decides whether the server sinks or echoes.
bool Connect(Harnessed& h, ConnectionType type, PortNumber port,
             bool echo, std::atomic_uint32_t* server_received,
             std::atomic_uint32_t* client_replies,
             bench::Clock::duration* connect_time) {
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(10), type};
  // Only the server configures these; the client follows whatever it selects.
  server_config.child_options.common.encryption = g_profile.encryption;
  server_config.child_options.common.compression = g_profile.compression;
  // Queue bounds raised out of the way, matching what the comparison rows do.
  // These are anti-flood limits, not tuning: a full inbox drops arrivals and a
  // full send queue refuses, so at the defaults the table would partly measure
  // whichever library shipped the smaller buffer. The congestion window and
  // max_datagrams_in_flight are deliberately left alone, since those are
  // protocol behavior and the impaired runs exist to show what they really do.
  ApplyBenchQueueBounds(server_config.child_options);
  h.server = std::make_unique<Server>(server_config);
  h.server->SetEventCallback([&, echo](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&, echo](IncomingClientConnectedEvent& ev) {
          ev.session()->SetCodec(MakeCodec());
          if (echo) {
            ev.session()->SetHandler(std::make_shared<EchoHandler>(ev.session()));
          } else {
            ev.session()->SetHandler(
                std::make_shared<SinkHandler>(server_received));
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
  ApplyBenchQueueBounds(client_config.options);
  h.client = std::make_unique<Client>(client_config);
  h.client->SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&](ClientConnectedToServerEvent& ev) {
          ev.session()->SetCodec(MakeCodec());
          ev.session()->SetHandler(std::make_shared<ReplyHandler>(client_replies));
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

// A port picked by FreePort() can be taken by another process before the server
// binds it, which shows up as a spurious "FAILED to connect" row. Retrying with
// a fresh port keeps a comparison table from silently losing a case.
bool ConnectWithRetry(Harnessed& h, ConnectionType type, bool echo,
                      std::atomic_uint32_t* server_received,
                      std::atomic_uint32_t* client_replies,
                      bench::Clock::duration* connect_time) {
  constexpr int kAttempts = 5;
  for (int attempt = 0; attempt < kAttempts; attempt++) {
    if (Connect(h, type, FreePort(), echo, server_received, client_replies,
                connect_time)) {
      return true;
    }
    Teardown(h);
    h.ready = false;
  }
  return false;
}

void RunThroughput(ConnectionType type, const bench::Workload& w) {
  if (type == ConnectionType::TCP && !TCPCanCarry(w.payload_bytes)) {
    std::printf("%-10s %-6s throughput %-6s  unsupported (exceeds ZNET_MAX_BUFFER_SIZE framing)\n",
                LibraryName().c_str(), TransportName(type), w.name);
    return;
  }
  std::atomic_uint32_t received{0};
  std::atomic_uint32_t replies{0};
  bench::Clock::duration connect_time{};
  Harnessed h;
  if (!ConnectWithRetry(h, type, /*echo=*/false, &received, &replies,
                        &connect_time)) {
    std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n",
                LibraryName().c_str(), TransportName(type), w.name);
    Teardown(h);
    return;
  }

  const std::string payload = bench::MakePayload(w.payload_bytes);
  uint32_t seq = 0;
  uint32_t counted = 0;
  auto start = bench::Clock::now();
  // the shared loop, same as every comparison library uses, so the send pacing
  // and the 4096-message backlog bound are identical across the table. Handing
  // znet the whole workload up front instead would measure a queue no other row
  // is allowed to build.
  uint32_t delivered = bench::RunThroughputLoop(
      w,
      [&]() {
        if (!h.client_session->IsAlive()) {
          return false;
        }
        auto packet = std::make_shared<BenchPacket>();
        packet->seq = seq;
        packet->payload = payload;
        // Send refuses when the transport's outbound queue is full; that is the
        // backpressure signal the loop drains on.
        if (!h.client_session->SendPacket(packet)) {
          return false;
        }
        seq++;
        return true;
      },
      [&]() {
        // znet services its own sockets on its own threads, so unlike the
        // libraries the application drives, there is nothing to pump here; the
        // loop only needs to be told how many arrived since it last asked.
        uint32_t now = received.load();
        uint32_t progress = now - counted;
        counted = now;
        return progress;
      });
  auto elapsed = bench::Clock::now() - start;

  bench::ReportThroughput(LibraryName().c_str(), TransportName(type), w,
                          delivered, elapsed);
  // ZNET_BENCH_METRICS=1 adds the session's protocol counters after each row,
  // so a run's throughput can be correlated with the state that produced it.
  // Off by default: the table is meant to stay diffable.
  if (getenv("ZNET_BENCH_METRICS") != nullptr && h.client_session) {
    SessionMetrics m = h.client_session->metrics();
    std::printf("%-10s %-6s metrics    %-6s  mtu %5u  cwnd %5u  dgram_tx %8llu  "
                "rtx %6llu  nak_rx %5llu  in_drop %5llu  reasm_drop %5llu  "
                "srtt %6u us  rtt_min %6u us  rto %7u us\n",
                LibraryName().c_str(), TransportName(type), w.name,
                m.zdt.mtu, m.zdt.cwnd,
                static_cast<unsigned long long>(m.zdt.datagrams_sent),
                static_cast<unsigned long long>(m.zdt.retransmits),
                static_cast<unsigned long long>(m.zdt.naks_received),
                static_cast<unsigned long long>(m.zdt.inbound_dropped),
                static_cast<unsigned long long>(m.zdt.reassemblies_dropped),
                m.zdt.srtt_us, m.zdt.rtt_min_us, m.zdt.rto_us);
  }
  Teardown(h);
}

void RunLatency(ConnectionType type, const bench::Workload& w) {
  std::atomic_uint32_t received{0};
  std::atomic_uint32_t replies{0};
  bench::Clock::duration connect_time{};
  Harnessed h;
  if (!ConnectWithRetry(h, type, /*echo=*/true, &received, &replies,
                        &connect_time)) {
    std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n",
                LibraryName().c_str(), TransportName(type), w.name);
    Teardown(h);
    return;
  }
  bench::ReportConnect(LibraryName().c_str(), TransportName(type), connect_time);

  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<double> samples;
  samples.reserve(w.messages);
  for (uint32_t i = 0; i < w.messages; i++) {
    uint32_t before = replies.load();
    auto sent = bench::Clock::now();
    auto packet = std::make_shared<BenchPacket>();
    packet->seq = i;
    packet->payload = payload;
    if (!h.client_session->SendPacket(packet)) {
      break;
    }
    auto deadline = sent + std::chrono::seconds(5);
    while (replies.load() == before && bench::Clock::now() < deadline &&
           h.client_session->IsAlive()) {
      std::this_thread::yield();
    }
    if (replies.load() == before) {
      break;  // timed out, stop rather than record a bogus sample
    }
    samples.push_back(
        std::chrono::duration<double, std::micro>(bench::Clock::now() - sent)
            .count());
  }
  bench::ReportLatency(LibraryName().c_str(), TransportName(type), w,
                       bench::Percentiles(samples));
  Teardown(h);
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
  bench::Note("default = AES + zstd as shipped; raw = both off, which is what");
  bench::Note("ENet and RakNet do. See README.md.");

  // Narrow the run to one transport and drop the latency case, for profiling.
  // A full run is dominated by the TCP latency case, two thousand tick-bound
  // round trips spent spinning, which buries the throughput path.
  //   ZNET_BENCH_TRANSPORT=zdt ZNET_BENCH_CASE=8KB ZNET_BENCH_SKIP_LATENCY=1
  const char* only_transport = std::getenv("ZNET_BENCH_TRANSPORT");
  const char* only_case = std::getenv("ZNET_BENCH_CASE");
  const bool skip_latency = std::getenv("ZNET_BENCH_SKIP_LATENCY") != nullptr;

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
    }
  }

  Cleanup();
  return 0;
}
