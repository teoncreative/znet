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
// RakNet runs its own threads, so only Receive() is polled; structurally the
// closest comparison to znet. It sends plaintext and does not compress;
// `znet-raw` is the like-for-like row. See benchmarks/README.md.
//

#include "common/congestion.h"
#include "common/harness.h"
#include "common/impairment.h"

#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "RakNetTypes.h"
#include "RakNetSocket2.h"
#include "BitStream.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace RakNet;

constexpr unsigned short kPortBase = 47200;
// Anything at or above ID_USER_PACKET_ENUM is ours to define.
constexpr unsigned char kBenchMessage = ID_USER_PACKET_ENUM + 1;
// bulk on ordering channel 0, probe on 1: independently ordered
constexpr char kBulkChannel = 0;
constexpr char kProbeChannel = 1;

// A port per case, never reused: Shutdown(100) returns before the peer's last
// datagrams drain, and the next case binding the same port inherits them.
unsigned short g_next_port = kPortBase;
unsigned short NextPort() {
  return g_next_port++;
}

bench::Impairment g_impair;

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

// RakNet creates its sockets with a 256 KB receive and 16 KB(!) send buffer
// (RNS2_Berkley::SetSocketOptions). Raise them like every other library gets,
// so the table measures the protocol rather than a 2005-era buffer choice.
void RaiseSocketBuffers(RakPeerInterface* peer) {
  constexpr int kBytes = 16 * 1024 * 1024;
  DataStructures::List<RakNetSocket2*> sockets;
  peer->GetSockets(sockets);
  for (unsigned i = 0; i < sockets.Size(); i++) {
    RNS2Type type = sockets[i]->GetSocketType();
    if (type != RNS2T_WINDOWS && type != RNS2T_LINUX) {
      continue;  // not Berkley-backed; no raw fd to reach
    }
    RNS2Socket fd = static_cast<RNS2_Berkley*>(sockets[i])->GetSocket();
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&kBytes), sizeof(kBytes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&kBytes), sizeof(kBytes));
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
  RaiseSocketBuffers(p.server);
  RaiseSocketBuffers(p.client);

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

// Send returns 0 on refusal; reporting that as success would advance the
// harness's backlog accounting for a message that never entered the protocol.
bool SendOrdered(RakPeerInterface* peer, const std::string& data, char channel,
                 const SystemAddress& to) {
  return peer->Send(data.data(), static_cast<int>(data.size()), HIGH_PRIORITY,
                    RELIABLE_ORDERED, channel, to, false) != 0;
}

// RELIABLE_ORDERED matches znet's channel 0 default.
void Throughput(const bench::Workload& w) {
  std::string payload = bench::MakePayload(w.payload_bytes);
  payload[0] = static_cast<char>(kBenchMessage);
  std::vector<bench::LoopResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    Pair p;
    bench::Clock::duration connect_time{};
    if (!Connect(p, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", "raknet",
                  "RakNet", w.name);
      DestroyPair(p);
      continue;
    }

    reps.push_back(bench::RunThroughputLoop(
        w,
        [&]() {
          return SendOrdered(p.client, payload, kBulkChannel, p.server_addr);
        },
        [&]() {
          uint32_t got = 0;
          for (Packet* packet = p.server->Receive(); packet;
               p.server->DeallocatePacket(packet),
                       packet = p.server->Receive()) {
            if (packet->data[0] == kBenchMessage) {
              got++;
            }
          }
          for (Packet* packet = p.client->Receive(); packet;
               p.client->DeallocatePacket(packet),
                       packet = p.client->Receive()) {
          }
          return got;
        },
        bench::ThroughputWarmup(g_impair)));
    DestroyPair(p);
  }
  bench::ReportThroughput("raknet", "RakNet", w, reps);
}

void Latency(const bench::Workload& w) {
  std::string payload = bench::MakePayload(w.payload_bytes);
  payload[0] = static_cast<char>(kBenchMessage);
  std::vector<std::vector<double>> rep_samples;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    Pair p;
    bench::Clock::duration connect_time{};
    if (!Connect(p, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", "raknet",
                  "RakNet", w.name);
      DestroyPair(p);
      continue;
    }
    if (rep == 0) {
      bench::ReportConnect("raknet", "RakNet", connect_time);
    }

    rep_samples.push_back(bench::RunLatencyLoop(
        w,
        [&]() {
          return SendOrdered(p.client, payload, kBulkChannel, p.server_addr);
        },
        [&]() {
          bool echoed = false;
          for (Packet* packet = p.server->Receive(); packet;
               p.server->DeallocatePacket(packet),
                       packet = p.server->Receive()) {
            if (packet->data[0] == kBenchMessage) {
              p.server->Send(reinterpret_cast<const char*>(packet->data),
                             static_cast<int>(packet->length), HIGH_PRIORITY,
                             RELIABLE_ORDERED, kBulkChannel,
                             packet->systemAddress, false);
            }
          }
          for (Packet* packet = p.client->Receive(); packet;
               p.client->DeallocatePacket(packet),
                       packet = p.client->Receive()) {
            if (packet->data[0] == kBenchMessage) {
              echoed = true;
            }
          }
          return echoed;
        }));
    DestroyPair(p);
  }
  bench::ReportLatency("raknet", "RakNet", w, rep_samples);
}

// Bulk on ordering channel 0, probe on 1; the server echoes probe-sized arrivals.
void Congestion(const bench::CongestionCase& c) {
  std::string bulk = bench::MakePayload(c.bulk_bytes);
  std::string probe = bench::MakePayload(c.probe_bytes);
  bulk[0] = static_cast<char>(kBenchMessage);
  probe[0] = static_cast<char>(kBenchMessage);
  std::vector<bench::CongestionResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    Pair p;
    bench::Clock::duration connect_time{};
    if (!Connect(p, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s congestion %-6s  FAILED to connect\n", "raknet",
                  "RakNet", c.name);
      DestroyPair(p);
      continue;
    }

    reps.push_back(bench::RunCongestionLoop(
        c,
        [&]() {
          return SendOrdered(p.client, bulk, kBulkChannel, p.server_addr);
        },
        [&]() {
          return SendOrdered(p.client, probe, kProbeChannel, p.server_addr);
        },
        [&]() {
          bench::PumpCounts counts;
          for (Packet* packet = p.server->Receive(); packet;
               p.server->DeallocatePacket(packet),
                       packet = p.server->Receive()) {
            if (packet->data[0] != kBenchMessage) {
              continue;
            }
            if (packet->length == c.probe_bytes) {
              p.server->Send(reinterpret_cast<const char*>(packet->data),
                             static_cast<int>(packet->length), HIGH_PRIORITY,
                             RELIABLE_ORDERED, kProbeChannel,
                             packet->systemAddress, false);
            } else {
              counts.bulk++;
            }
          }
          for (Packet* packet = p.client->Receive(); packet;
               p.client->DeallocatePacket(packet),
                       packet = p.client->Receive()) {
            if (packet->data[0] == kBenchMessage) {
              counts.probes++;
            }
          }
          return counts;
        }));
    DestroyPair(p);
  }
  bench::ReportCongestionCase("raknet", "RakNet", c, reps, "channel");
}

}  // namespace

int main() {
  std::printf("RakNet 4.x (facebookarchive master)\n");
  bench::Note("plaintext, no compression; RELIABLE_ORDERED on channel 0");
  bench::Note("socket buffers raised from RakNet's 256 KB recv / 16 KB send");
  bench::Note("defaults, matching what the other libraries get.");
  g_impair = bench::Impairment::FromEnv();
  bench::NoteImpairment(g_impair);
  bench::AnnounceRunSettings();
  bench::PrintHeader("raknet", "RakNet");
  for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
    Throughput(w);
  }
  Latency(bench::ImpairedLatencyWorkload(g_impair));
  if (std::getenv("ZNET_BENCH_SKIP_CONGESTION") == nullptr) {
    for (const auto& c : bench::DefaultCongestionCases()) {
      Congestion(c);
    }
  }
  return 0;
}
