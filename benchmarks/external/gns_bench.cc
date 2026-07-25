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
// GNS is the closest comparison to znet in feature terms: it encrypts by
// default (AES-GCM) and does its own congestion control, so unlike ENet and
// RakNet it is not getting a free pass on crypto. It runs its own threads and
// is polled, like RakNet.
//

#include "common/harness.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16 kPort = 47300;

// GNS reports connection state through a callback with no user-data slot, so
// the harness keeps the handles it needs in one place.
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
      // inbound half: accept it and file it under the poll group
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

// Each case gets its own port: GNS keeps a closed connection around briefly for
// lingering traffic, and reusing the port immediately fails the next connect.
void Throughput(const bench::Workload& w, uint16 port) {
  State s;
  g_state = &s;
  bench::Clock::duration connect_time{};
  if (!Connect(s, port, &connect_time)) {
    std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", "gns", "GNS",
                w.name);
    Teardown(s);
    g_state = nullptr;
    return;
  }

  const std::string payload = bench::MakePayload(w.payload_bytes);
  SteamNetworkingMessage_t* messages[128];
  auto start = bench::Clock::now();
  uint32_t received = bench::RunThroughputLoop(
      w,
      [&]() {
        int64 out = 0;
        return s.sockets->SendMessageToConnection(
                   s.client_conn, payload.data(),
                   static_cast<uint32>(payload.size()),
                   k_nSteamNetworkingSend_Reliable, &out) == k_EResultOK;
      },
      [&]() {
        s.sockets->RunCallbacks();
        int count =
            s.sockets->ReceiveMessagesOnPollGroup(s.poll_group, messages, 128);
        for (int i = 0; i < count; i++) {
          messages[i]->Release();
        }
        return static_cast<uint32_t>(count);
      });
  auto elapsed = bench::Clock::now() - start;
  bench::ReportThroughput("gns", "GNS", w, received, elapsed);
  Teardown(s);
  g_state = nullptr;
}

void Latency(const bench::Workload& w) {
  State s;
  g_state = &s;
  bench::Clock::duration connect_time{};
  if (!Connect(s, kPort + 100, &connect_time)) {
    std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", "gns", "GNS",
                w.name);
    Teardown(s);
    g_state = nullptr;
    return;
  }
  bench::ReportConnect("gns", "GNS", connect_time);

  const std::string payload = bench::MakePayload(w.payload_bytes);
  SteamNetworkingMessage_t* messages[8];
  std::vector<double> samples = bench::RunLatencyLoop(
      w,
      [&]() {
        int64 out = 0;
        return s.sockets->SendMessageToConnection(
                   s.client_conn, payload.data(),
                   static_cast<uint32>(payload.size()),
                   k_nSteamNetworkingSend_Reliable, &out) == k_EResultOK;
      },
      [&]() {
        bool echoed = false;
        int64 out = 0;
        s.sockets->RunCallbacks();
        int count =
            s.sockets->ReceiveMessagesOnPollGroup(s.poll_group, messages, 8);
        for (int j = 0; j < count; j++) {
          // server side: bounce it back
          s.sockets->SendMessageToConnection(
              s.server_conn, messages[j]->GetData(), messages[j]->GetSize(),
              k_nSteamNetworkingSend_Reliable, &out);
          messages[j]->Release();
        }
        count = s.sockets->ReceiveMessagesOnConnection(s.client_conn, messages, 8);
        for (int j = 0; j < count; j++) {
          echoed = true;
          messages[j]->Release();
        }
        return echoed;
      });
  bench::ReportLatency("gns", "GNS", w, bench::Percentiles(samples));
  Teardown(s);
  g_state = nullptr;
}

}  // namespace

int main() {
  SteamNetworkingErrMsg err;
  if (!GameNetworkingSockets_Init(nullptr, err)) {
    std::fprintf(stderr, "failed to initialize GameNetworkingSockets: %s\n", err);
    return 1;
  }
  // GNS ships a conservative default send rate (~256 KB/s) meant for the open
  // internet. Left alone it pins every payload size to the same byte rate and
  // the benchmark measures the rate limiter instead of the protocol, so raise
  // it well past what loopback will need.
  constexpr int32 kSendRate = 512 * 1024 * 1024;  // bytes/sec
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_SendRateMin, kSendRate);
  SteamNetworkingUtils()->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_SendRateMax, kSendRate);

  std::printf("GameNetworkingSockets 1.6.0\n");
  bench::Note("encrypted (AES-GCM) like znet, no compression; reliable stream");
  bench::Note("send rate limit raised from the ~256 KB/s default for loopback");
  bench::PrintHeader("gns", "GNS");
  uint16 port = kPort;
  for (const auto& w : bench::DefaultThroughputWorkloads()) {
    Throughput(w, port++);
  }
  Latency(bench::DefaultLatencyWorkload());
  GameNetworkingSockets_Kill();
  return 0;
}
