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
// ENet is single-threaded and poll-driven: enet_host_service() is what moves
// packets, so both ends are serviced in a tight loop with a zero timeout. That
// is ENet's intended usage and it has no tick rate of its own, which is worth
// keeping in mind when comparing against znet's tick-based server.
//
// ENet sends plaintext and does not compress. znet encrypts and compresses
// every packet by default. See benchmarks/README.md.
//

#include "common/harness.h"
#include "common/impairment.h"

#include <enet/enet.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr enet_uint16 kPort = 47100;
constexpr size_t kChannels = 2;

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

// Services both hosts until the client peer is connected, or the deadline
// passes. Returns the time the handshake took.
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

// Reliable + ordered, to match znet's channel 0 default.
void Throughput(const bench::Workload& w) {
  Pair p;
  bench::Clock::duration connect_time{};
  if (!Connect(p, kPort, &connect_time)) {
    std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", "enet",
                "ENet", w.name);
    DestroyPair(p);
    return;
  }

  const std::string payload = bench::MakePayload(w.payload_bytes);
  auto start = bench::Clock::now();
  uint32_t received = bench::RunThroughputLoop(
      w,
      [&]() {
        ENetPacket* packet = enet_packet_create(payload.data(), payload.size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(p.peer, 0, packet) < 0) {
          enet_packet_destroy(packet);
          return false;
        }
        return true;
      },
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
      });
  auto elapsed = bench::Clock::now() - start;
  bench::ReportThroughput("enet", "ENet", w, received, elapsed);
  DestroyPair(p);
}

void Latency(const bench::Workload& w) {
  Pair p;
  bench::Clock::duration connect_time{};
  if (!Connect(p, kPort + 1, &connect_time)) {
    std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", "enet",
                "ENet", w.name);
    DestroyPair(p);
    return;
  }
  bench::ReportConnect("enet", "ENet", connect_time);

  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<double> samples = bench::RunLatencyLoop(
      w,
      [&]() {
        ENetPacket* packet = enet_packet_create(payload.data(), payload.size(),
                                                ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(p.peer, 0, packet) < 0) {
          enet_packet_destroy(packet);
          return false;
        }
        return true;
      },
      [&]() {
        bool echoed = false;
        ENetEvent event;
        while (enet_host_service(p.server, &event, 0) > 0) {
          if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            // echo straight back on the same channel
            ENetPacket* reply =
                enet_packet_create(event.packet->data, event.packet->dataLength,
                                   ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(event.peer, 0, reply);
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
      });
  bench::ReportLatency("enet", "ENet", w, bench::Percentiles(samples));
  DestroyPair(p);
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
  g_impair = bench::Impairment::FromEnv();
  bench::NoteImpairment(g_impair);
  bench::PrintHeader("enet", "ENet");
  for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
    Throughput(w);
  }
  Latency(bench::ImpairedLatencyWorkload(g_impair));
  enet_deinitialize();
  return 0;
}
