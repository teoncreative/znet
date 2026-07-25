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
// RakNet over loopback, run the same way as znet-bench so the rows line up.
//
// RakNet runs its own internal threads, so unlike ENet the benchmark only
// polls Receive() rather than driving the protocol itself. It is the closest
// structural comparison to znet: a congestion window, reliability channels and
// an ordered stream, with the library owning its threading.
//
// RakNet sends plaintext here and does not compress. znet encrypts and
// compresses every packet by default. See benchmarks/README.md.
//

#include "common/harness.h"

#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "RakNetTypes.h"
#include "BitStream.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace RakNet;

constexpr unsigned short kServerPort = 47200;
// Anything at or above ID_USER_PACKET_ENUM is ours to define.
constexpr unsigned char kBenchMessage = ID_USER_PACKET_ENUM + 1;

struct Pair {
  RakPeerInterface* server = nullptr;
  RakPeerInterface* client = nullptr;
  SystemAddress server_addr;
};

void DestroyPair(Pair& p) {
  if (p.client) {
    p.client->Shutdown(100);
    RakPeerInterface::DestroyInstance(p.client);
    p.client = nullptr;
  }
  if (p.server) {
    p.server->Shutdown(100);
    RakPeerInterface::DestroyInstance(p.server);
    p.server = nullptr;
  }
}

bool Connect(Pair& p, unsigned short port,
             bench::Clock::duration* connect_time) {
  p.server = RakPeerInterface::GetInstance();
  SocketDescriptor server_sd(port, nullptr);
  if (p.server->Startup(4, &server_sd, 1) != RAKNET_STARTED) {
    return false;
  }
  p.server->SetMaximumIncomingConnections(4);

  p.client = RakPeerInterface::GetInstance();
  SocketDescriptor client_sd(0, nullptr);
  if (p.client->Startup(1, &client_sd, 1) != RAKNET_STARTED) {
    return false;
  }

  auto start = bench::Clock::now();
  if (p.client->Connect("127.0.0.1", port, nullptr, 0) !=
      CONNECTION_ATTEMPT_STARTED) {
    return false;
  }

  bool connected = false;
  auto deadline = bench::Clock::now() + std::chrono::seconds(10);
  while (!connected && bench::Clock::now() < deadline) {
    for (Packet* packet = p.client->Receive(); packet;
         p.client->DeallocatePacket(packet), packet = p.client->Receive()) {
      if (packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED) {
        p.server_addr = packet->systemAddress;
        connected = true;
      }
    }
    for (Packet* packet = p.server->Receive(); packet;
         p.server->DeallocatePacket(packet), packet = p.server->Receive()) {
    }
  }
  *connect_time = bench::Clock::now() - start;
  return connected;
}

// RELIABLE_ORDERED matches znet's channel 0 default.
void Throughput(const bench::Workload& w) {
  Pair p;
  bench::Clock::duration connect_time{};
  if (!Connect(p, kServerPort, &connect_time)) {
    std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", "raknet",
                "RakNet", w.name);
    DestroyPair(p);
    return;
  }

  std::string payload = bench::MakePayload(w.payload_bytes);
  payload[0] = static_cast<char>(kBenchMessage);

  auto start = bench::Clock::now();
  uint32_t received = bench::RunThroughputLoop(
      w,
      [&]() {
        p.client->Send(payload.data(), static_cast<int>(payload.size()),
                       HIGH_PRIORITY, RELIABLE_ORDERED, 0, p.server_addr,
                       false);
        return true;
      },
      [&]() {
        uint32_t got = 0;
        for (Packet* packet = p.server->Receive(); packet;
             p.server->DeallocatePacket(packet), packet = p.server->Receive()) {
          if (packet->data[0] == kBenchMessage) {
            got++;
          }
        }
        for (Packet* packet = p.client->Receive(); packet;
             p.client->DeallocatePacket(packet), packet = p.client->Receive()) {
        }
        return got;
      });
  auto elapsed = bench::Clock::now() - start;
  bench::ReportThroughput("raknet", "RakNet", w, received, elapsed);
  DestroyPair(p);
}

void Latency(const bench::Workload& w) {
  Pair p;
  bench::Clock::duration connect_time{};
  if (!Connect(p, kServerPort + 1, &connect_time)) {
    std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", "raknet",
                "RakNet", w.name);
    DestroyPair(p);
    return;
  }
  bench::ReportConnect("raknet", "RakNet", connect_time);

  std::string payload = bench::MakePayload(w.payload_bytes);
  payload[0] = static_cast<char>(kBenchMessage);

  std::vector<double> samples = bench::RunLatencyLoop(
      w,
      [&]() {
        p.client->Send(payload.data(), static_cast<int>(payload.size()),
                       HIGH_PRIORITY, RELIABLE_ORDERED, 0, p.server_addr,
                       false);
        return true;
      },
      [&]() {
        bool echoed = false;
        for (Packet* packet = p.server->Receive(); packet;
             p.server->DeallocatePacket(packet), packet = p.server->Receive()) {
          if (packet->data[0] == kBenchMessage) {
            p.server->Send(reinterpret_cast<const char*>(packet->data),
                           static_cast<int>(packet->length), HIGH_PRIORITY,
                           RELIABLE_ORDERED, 0, packet->systemAddress, false);
          }
        }
        for (Packet* packet = p.client->Receive(); packet;
             p.client->DeallocatePacket(packet), packet = p.client->Receive()) {
          if (packet->data[0] == kBenchMessage) {
            echoed = true;
          }
        }
        return echoed;
      });
  bench::ReportLatency("raknet", "RakNet", w, bench::Percentiles(samples));
  DestroyPair(p);
}

}  // namespace

int main() {
  std::printf("RakNet 4.x (facebookarchive master)\n");
  bench::Note("plaintext, no compression; RELIABLE_ORDERED on channel 0");
  bench::PrintHeader("raknet", "RakNet");
  for (const auto& w : bench::DefaultThroughputWorkloads()) {
    Throughput(w);
  }
  Latency(bench::DefaultLatencyWorkload());
  return 0;
}
