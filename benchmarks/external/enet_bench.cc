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
// ENet over loopback, run the same way as znet-bench so the rows line up.
//
// ENet is single-threaded and poll-driven; both ends are serviced in a tight
// zero-timeout loop, its intended usage. It sends plaintext and does not
// compress; `znet-raw` is the like-for-like row. See benchmarks/README.md.
//

#include "common/congestion.h"
#include "common/harness.h"
#include "common/impairment.h"

#include <enet/enet.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr enet_uint16 kPortBase = 47100;
constexpr size_t kChannels = 2;
// bulk on 0, probe on 1: separate sequence spaces, no head-of-line coupling
constexpr enet_uint8 kBulkChannel = 0;
constexpr enet_uint8 kProbeChannel = 1;

// A port per case, never reused: a destroyed host's last datagrams can still
// be in flight, and the next case binding the same port inherits them.
enet_uint16 g_next_port = kPortBase;
enet_uint16 NextPort() {
  return g_next_port++;
}

bench::Impairment g_impair;

struct Pair {
  ENetHost* server = nullptr;
  ENetHost* client = nullptr;
  ENetPeer* peer = nullptr;
};

void DestroyPair(Pair& p) {
  if (p.client) {
    enet_host_destroy(p.client);
    p.client = nullptr;
  }
  if (p.server) {
    enet_host_destroy(p.server);
    p.server = nullptr;
  }
  p.peer = nullptr;
}

// ENet creates its sockets with 256 KB buffers (ENET_HOST_*_BUFFER_SIZE).
// Raise them like every other library gets, so the table measures the
// protocol rather than whoever shipped the smaller socket buffer.
void RaiseSocketBuffers(ENetHost* host) {
  constexpr int kBytes = 16 * 1024 * 1024;
  enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, kBytes);
  enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, kBytes);
}

bool Connect(Pair& p, enet_uint16 port, bench::Clock::duration* connect_time) {
  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  address.port = port;
  p.server = enet_host_create(&address, 4, kChannels, 0, 0);
  if (!p.server) {
    return false;
  }
  p.client = enet_host_create(nullptr, 1, kChannels, 0, 0);
  if (!p.client) {
    return false;
  }
  RaiseSocketBuffers(p.server);
  RaiseSocketBuffers(p.client);

  ENetAddress to{};
  enet_address_set_host(&to, "127.0.0.1");
  to.port = port;

  auto start = bench::Clock::now();
  p.peer = enet_host_connect(p.client, &to, kChannels, 0);
  if (!p.peer) {
    return false;
  }

  bool connected = false;
  auto deadline = bench::Clock::now() + std::chrono::seconds(10);
  while (!connected && bench::Clock::now() < deadline) {
    ENetEvent event;
    while (enet_host_service(p.client, &event, 0) > 0) {
      if (event.type == ENET_EVENT_TYPE_CONNECT) {
        connected = true;
      }
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
    }
    while (enet_host_service(p.server, &event, 0) > 0) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
    }
  }
  *connect_time = bench::Clock::now() - start;
  return connected;
}

bool SendReliable(ENetPeer* peer, enet_uint8 channel, const std::string& data) {
  ENetPacket* packet =
      enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
  if (enet_peer_send(peer, channel, packet) < 0) {
    enet_packet_destroy(packet);
    return false;
  }
  return true;
}

// Reliable + ordered, to match znet's channel 0 default.
void Throughput(const bench::Workload& w) {
  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<bench::LoopResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    Pair p;
    bench::Clock::duration connect_time{};
    if (!Connect(p, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", "enet",
                  "ENet", w.name);
      DestroyPair(p);
      continue;
    }

    reps.push_back(bench::RunThroughputLoop(
        w,
        [&]() { return SendReliable(p.peer, kBulkChannel, payload); },
        [&]() {
          uint32_t got = 0;
          ENetEvent event;
          while (enet_host_service(p.client, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
              enet_packet_destroy(event.packet);
            }
          }
          while (enet_host_service(p.server, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
              got++;
              enet_packet_destroy(event.packet);
            }
          }
          return got;
        },
        bench::ThroughputWarmup(g_impair)));
    DestroyPair(p);
  }
  bench::ReportThroughput("enet", "ENet", w, reps);
}

void Latency(const bench::Workload& w) {
  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<std::vector<double>> rep_samples;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    Pair p;
    bench::Clock::duration connect_time{};
    if (!Connect(p, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", "enet",
                  "ENet", w.name);
      DestroyPair(p);
      continue;
    }
    if (rep == 0) {
      bench::ReportConnect("enet", "ENet", connect_time);
    }

    rep_samples.push_back(bench::RunLatencyLoop(
        w,
        [&]() { return SendReliable(p.peer, kBulkChannel, payload); },
        [&]() {
          bool echoed = false;
          ENetEvent event;
          while (enet_host_service(p.server, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
              ENetPacket* reply = enet_packet_create(
                  event.packet->data, event.packet->dataLength,
                  ENET_PACKET_FLAG_RELIABLE);
              if (enet_peer_send(event.peer, kBulkChannel, reply) < 0) {
                enet_packet_destroy(reply);
              }
              enet_packet_destroy(event.packet);
            }
          }
          while (enet_host_service(p.client, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
              echoed = true;
              enet_packet_destroy(event.packet);
            }
          }
          return echoed;
        }));
    DestroyPair(p);
  }
  bench::ReportLatency("enet", "ENet", w, rep_samples);
}

// Bulk on channel 0, probe on channel 1; the server echoes probe-sized arrivals.
void Congestion(const bench::CongestionCase& c) {
  const std::string bulk = bench::MakePayload(c.bulk_bytes);
  const std::string probe = bench::MakePayload(c.probe_bytes);
  std::vector<bench::CongestionResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    Pair p;
    bench::Clock::duration connect_time{};
    if (!Connect(p, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s congestion %-6s  FAILED to connect\n", "enet",
                  "ENet", c.name);
      DestroyPair(p);
      continue;
    }

    reps.push_back(bench::RunCongestionLoop(
        c,
        [&]() { return SendReliable(p.peer, kBulkChannel, bulk); },
        [&]() { return SendReliable(p.peer, kProbeChannel, probe); },
        [&]() {
          bench::PumpCounts counts;
          ENetEvent event;
          while (enet_host_service(p.server, &event, 0) > 0) {
            if (event.type != ENET_EVENT_TYPE_RECEIVE) {
              continue;
            }
            if (event.packet->dataLength == c.probe_bytes) {
              ENetPacket* reply = enet_packet_create(
                  event.packet->data, event.packet->dataLength,
                  ENET_PACKET_FLAG_RELIABLE);
              if (enet_peer_send(event.peer, kProbeChannel, reply) < 0) {
                enet_packet_destroy(reply);
              }
            } else {
              counts.bulk++;
            }
            enet_packet_destroy(event.packet);
          }
          while (enet_host_service(p.client, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
              counts.probes++;
              enet_packet_destroy(event.packet);
            }
          }
          return counts;
        }));
    DestroyPair(p);
  }
  bench::ReportCongestionCase("enet", "ENet", c, reps, "channel");
}

}  // namespace

int main() {
  if (enet_initialize() != 0) {
    std::fprintf(stderr, "failed to initialize ENet\n");
    return 1;
  }
  std::printf("ENet %d.%d.%d\n", ENET_VERSION_MAJOR, ENET_VERSION_MINOR,
              ENET_VERSION_PATCH);
  bench::Note("plaintext, no compression; reliable+ordered on channel 0");
  bench::Note("socket buffers raised from ENet's 256 KB defaults, matching");
  bench::Note("what the other libraries get. The 64 KB protocol window stays.");
  g_impair = bench::Impairment::FromEnv();
  bench::NoteImpairment(g_impair);
  bench::AnnounceRunSettings();
  bench::PrintHeader("enet", "ENet");
  for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
    Throughput(w);
  }
  Latency(bench::ImpairedLatencyWorkload(g_impair));
  if (std::getenv("ZNET_BENCH_SKIP_CONGESTION") == nullptr) {
    for (const auto& c : bench::DefaultCongestionCases()) {
      Congestion(c);
    }
  }
  enet_deinitialize();
  return 0;
}
