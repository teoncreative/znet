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
// Valve GameNetworkingSockets over loopback, run the same way as znet-bench.
//
// The closest comparison in feature terms: encrypts by default (AES-GCM) and
// does its own congestion control. Runs its own threads and is polled, like
// RakNet. Two Nagle profiles are reported; see README.md.
//

#include "common/congestion.h"
#include "common/harness.h"
#include "common/impairment.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16 kPortBase = 47300;

// A port per case, never reused: GNS keeps a closed connection around briefly,
// and rebinding immediately fails the next connect.
uint16 g_next_port = kPortBase;
uint16 NextPort() {
  return g_next_port++;
}

// Library column name; distinguishes the two Nagle profiles in one table.
const char* g_library = "gns";

// The status callback has no user-data slot, so state is kept in one place.
struct State {
  ISteamNetworkingSockets* sockets = nullptr;
  HSteamListenSocket listen_socket = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group = k_HSteamNetPollGroup_Invalid;
  HSteamNetConnection server_conn = k_HSteamNetConnection_Invalid;
  HSteamNetConnection client_conn = k_HSteamNetConnection_Invalid;
  std::atomic_bool client_connected{false};
};

State* g_state = nullptr;

void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
  if (!g_state) {
    return;
  }
  switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
      // inbound half: accept and file under the poll group
      if (info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
        g_state->sockets->AcceptConnection(info->m_hConn);
        g_state->sockets->SetConnectionPollGroup(info->m_hConn,
                                                 g_state->poll_group);
        g_state->server_conn = info->m_hConn;
      }
      break;
    case k_ESteamNetworkingConnectionState_Connected:
      if (info->m_hConn == g_state->client_conn) {
        g_state->client_connected = true;
      }
      break;
    default:
      break;
  }
}

bench::Impairment g_impair;

bool Connect(State& s, uint16 port, bench::Clock::duration* connect_time) {
  s.sockets = SteamNetworkingSockets();
  s.poll_group = s.sockets->CreatePollGroup();

  SteamNetworkingIPAddr local{};
  local.Clear();
  local.m_port = port;

  SteamNetworkingConfigValue_t option{};
  option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                reinterpret_cast<void*>(OnConnectionStatusChanged));

  s.listen_socket = s.sockets->CreateListenSocketIP(local, 1, &option);
  if (s.listen_socket == k_HSteamListenSocket_Invalid) {
    return false;
  }

  SteamNetworkingIPAddr to{};
  to.Clear();
  to.SetIPv4(0x7f000001, port);  // 127.0.0.1

  auto start = bench::Clock::now();
  s.client_conn = s.sockets->ConnectByIPAddress(to, 1, &option);
  if (s.client_conn == k_HSteamNetConnection_Invalid) {
    return false;
  }

  auto deadline = bench::Clock::now() + std::chrono::seconds(10);
  while (!s.client_connected.load() && bench::Clock::now() < deadline) {
    s.sockets->RunCallbacks();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  *connect_time = bench::Clock::now() - start;
  return s.client_connected.load();
}

void Teardown(State& s) {
  if (s.sockets) {
    if (s.client_conn != k_HSteamNetConnection_Invalid) {
      s.sockets->CloseConnection(s.client_conn, 0, nullptr, false);
    }
    if (s.server_conn != k_HSteamNetConnection_Invalid) {
      s.sockets->CloseConnection(s.server_conn, 0, nullptr, false);
    }
    if (s.listen_socket != k_HSteamListenSocket_Invalid) {
      s.sockets->CloseListenSocket(s.listen_socket);
    }
    if (s.poll_group != k_HSteamNetPollGroup_Invalid) {
      s.sockets->DestroyPollGroup(s.poll_group);
    }
  }
}

bool SendReliable(State& s, HSteamNetConnection conn, const void* data,
                  uint32 size) {
  int64 out = 0;
  return s.sockets->SendMessageToConnection(
             conn, data, size, k_nSteamNetworkingSend_Reliable, &out) ==
         k_EResultOK;
}

void Throughput(const bench::Workload& w) {
  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<bench::LoopResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    State s;
    g_state = &s;
    bench::Clock::duration connect_time{};
    if (!Connect(s, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", g_library,
                  "GNS", w.name);
      Teardown(s);
      g_state = nullptr;
      continue;
    }

    SteamNetworkingMessage_t* messages[128];
    reps.push_back(bench::RunThroughputLoop(
        w,
        [&]() {
          return SendReliable(s, s.client_conn, payload.data(),
                              static_cast<uint32>(payload.size()));
        },
        [&]() {
          s.sockets->RunCallbacks();
          int count = s.sockets->ReceiveMessagesOnPollGroup(s.poll_group,
                                                            messages, 128);
          for (int i = 0; i < count; i++) {
            messages[i]->Release();
          }
          return static_cast<uint32_t>(count);
        },
        bench::ThroughputWarmup(g_impair)));
    Teardown(s);
    g_state = nullptr;
  }
  bench::ReportThroughput(g_library, "GNS", w, reps);
}

void Latency(const bench::Workload& w) {
  const std::string payload = bench::MakePayload(w.payload_bytes);
  std::vector<std::vector<double>> rep_samples;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    State s;
    g_state = &s;
    bench::Clock::duration connect_time{};
    if (!Connect(s, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", g_library,
                  "GNS", w.name);
      Teardown(s);
      g_state = nullptr;
      continue;
    }
    if (rep == 0) {
      bench::ReportConnect(g_library, "GNS", connect_time);
    }

    SteamNetworkingMessage_t* messages[8];
    rep_samples.push_back(bench::RunLatencyLoop(
        w,
        [&]() {
          return SendReliable(s, s.client_conn, payload.data(),
                              static_cast<uint32>(payload.size()));
        },
        [&]() {
          bool echoed = false;
          s.sockets->RunCallbacks();
          int count =
              s.sockets->ReceiveMessagesOnPollGroup(s.poll_group, messages, 8);
          for (int j = 0; j < count; j++) {
            // server side: bounce it back
            SendReliable(s, s.server_conn, messages[j]->GetData(),
                         messages[j]->GetSize());
            messages[j]->Release();
          }
          count =
              s.sockets->ReceiveMessagesOnConnection(s.client_conn, messages, 8);
          for (int j = 0; j < count; j++) {
            echoed = true;
            messages[j]->Release();
          }
          return echoed;
        }));
    Teardown(s);
    g_state = nullptr;
  }
  bench::ReportLatency(g_library, "GNS", w, rep_samples);
}

// One reliable ordered stream, no channels: the probe shares the stream and is
// head-of-line blocked as well as queued. Reported as sep=none; not a
// controller comparison against a sep=channel row.
void Congestion(const bench::CongestionCase& c) {
  const std::string bulk = bench::MakePayload(c.bulk_bytes);
  const std::string probe = bench::MakePayload(c.probe_bytes);
  std::vector<bench::CongestionResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    State s;
    g_state = &s;
    bench::Clock::duration connect_time{};
    if (!Connect(s, NextPort(), &connect_time)) {
      std::printf("%-10s %-6s congestion %-6s  FAILED to connect\n", g_library,
                  "GNS", c.name);
      Teardown(s);
      g_state = nullptr;
      continue;
    }

    SteamNetworkingMessage_t* messages[128];
    reps.push_back(bench::RunCongestionLoop(
        c,
        [&]() {
          return SendReliable(s, s.client_conn, bulk.data(),
                              static_cast<uint32>(bulk.size()));
        },
        [&]() {
          return SendReliable(s, s.client_conn, probe.data(),
                              static_cast<uint32>(probe.size()));
        },
        [&]() {
          bench::PumpCounts counts;
          s.sockets->RunCallbacks();
          int count = s.sockets->ReceiveMessagesOnPollGroup(s.poll_group,
                                                            messages, 128);
          for (int i = 0; i < count; i++) {
            if (messages[i]->GetSize() == c.probe_bytes) {
              SendReliable(s, s.server_conn, messages[i]->GetData(),
                           messages[i]->GetSize());
            } else {
              counts.bulk++;
            }
            messages[i]->Release();
          }
          count = s.sockets->ReceiveMessagesOnConnection(s.client_conn,
                                                         messages, 128);
          for (int i = 0; i < count; i++) {
            counts.probes++;
            messages[i]->Release();
          }
          return counts;
        }));
    Teardown(s);
    g_state = nullptr;
  }
  bench::ReportCongestionCase(g_library, "GNS", c, reps, "none");
}

}  // namespace

int main() {
  SteamNetworkingErrMsg err;
  if (!GameNetworkingSockets_Init(nullptr, err)) {
    std::fprintf(stderr, "failed to initialize GameNetworkingSockets: %s\n", err);
    return 1;
  }
  // Raise everything reachable out of the way; the defaults are open-internet
  // anti-flood settings, and the receive-side ones drop rather than backpressure.
  // The SNP layer still clamps the send rate to 100 MiB/s internally.
  int32 send_rate = 0x10000000;  // bytes/sec, the largest accepted value
  if (const char* r = std::getenv("GNS_SEND_RATE")) {
    send_rate = std::atoi(r);
  }
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_SendRateMin, send_rate);
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_SendRateMax, send_rate);
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_SendBufferSize, 32 * 1024 * 1024);
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_RecvBufferSize, 256 * 1024 * 1024);
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_RecvBufferMessages, 1024 * 1024);
  // below RecvBufferSize, above the largest payload benchmarked
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_RecvMaxMessageSize, 16 * 1024 * 1024);

  std::printf("GameNetworkingSockets 1.6.0\n");
  bench::Note("encrypted (AES-GCM) like znet, no compression; reliable stream");
  bench::Note("send rate limit raised from the ~256 KB/s default for loopback");
  bench::Note("both Nagle settings are run: `gns` sends immediately as ZDT");
  bench::Note("does, `gns-nagle` keeps the 5ms coalescing GNS ships with.");
  g_impair = bench::Impairment::FromEnv();
  bench::NoteImpairment(g_impair);
  bench::AnnounceRunSettings();

  // Nagle=0 is like-for-like against znet on latency, but disables the
  // coalescing that pays at 64 B, the one case the send-rate clamp does not
  // pin. Run both; GNS_NAGLE_US overrides the second profile.
  int32 nagle_on_us = 5000;
  if (const char* n = std::getenv("GNS_NAGLE_US")) {
    nagle_on_us = std::atoi(n);
  }
  struct Profile {
    const char* name;
    int32 nagle_us;
  };
  const Profile profiles[] = {{"gns", 0}, {"gns-nagle", nagle_on_us}};

  for (const Profile& profile : profiles) {
    g_library = profile.name;
    // read at connection creation; every case makes its own connection
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_NagleTime, profile.nagle_us);
    bench::PrintHeader(g_library, "GNS");
    std::printf("  NagleTime = %d us\n", profile.nagle_us);
    for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
      Throughput(w);
    }
    Latency(bench::ImpairedLatencyWorkload(g_impair));
    for (const auto& c : bench::DefaultCongestionCases()) {
      Congestion(c);
    }
  }

  GameNetworkingSockets_Kill();
  return 0;
}
