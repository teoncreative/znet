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
// TCPTransportLayer keepalive and idle-timeout tests over a real loopback
// pair. The transports are driven by hand, the way a session worker would,
// with short timers so silence and pings both show inside a test's budget.
//

#include "znet/backends/tcp.h"
#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/init.h"
#include "znet/inet_addr.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/packet_serializer.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace znet;
using namespace znet::backends;

namespace {

// A connected loopback pair, both ends non-blocking as the transport expects.
struct SocketPair {
  SocketHandle a = kSocketInvalid;
  SocketHandle b = kSocketInvalid;
  bool ok = false;

  SocketPair() {
    SocketHandle listener = socket(AF_INET, SOCK_STREAM, 0);
    if (!IsValidSocketHandle(listener)) {
      return;
    }
    auto any = InetAddress::from("127.0.0.1", 0);
    if (bind(listener, any->handle_ptr(), any->addr_size()) != 0 ||
        listen(listener, 1) != 0) {
      CloseSocket(listener);
      return;
    }
    sockaddr_storage bound{};
    socklen_t len = static_cast<socklen_t>(sizeof(bound));
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
      CloseSocket(listener);
      return;
    }
    a = socket(AF_INET, SOCK_STREAM, 0);
    if (!IsValidSocketHandle(a) ||
        connect(a, reinterpret_cast<sockaddr*>(&bound), len) != 0) {
      CloseSocket(listener);
      return;
    }
    b = accept(listener, nullptr, nullptr);
    CloseSocket(listener);
    if (!IsValidSocketHandle(b)) {
      return;
    }
    SetSocketBlocking(a, false);
    SetSocketBlocking(b, false);
    ok = true;
  }
};

CommonOptions Timers(int keepalive_ms, int idle_ms) {
  CommonOptions common;
  common.keepalive_interval = std::chrono::milliseconds(keepalive_ms);
  common.idle_timeout = std::chrono::milliseconds(idle_ms);
  return common;
}

// Drives both ends the way their session workers would, for `ms`.
void Pump(TCPTransportLayer& a, TCPTransportLayer& b, int ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    a.Update();
    b.Update();
    while (a.Receive()) {
    }
    while (b.Receive()) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace

TEST(TCPKeepalive, SilenceClosesTheConnection) {
  ASSERT_EQ(Init(), Result::Success);
  SocketPair pair;
  ASSERT_TRUE(pair.ok);
  // a never speaks and b never pings, so b's idle timer is all that runs
  TCPTransportLayer a(pair.a, Timers(0, 0));
  TCPTransportLayer b(pair.b, Timers(0, 80));

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!b.IsClosed() && std::chrono::steady_clock::now() < deadline) {
    b.Update();
    b.Receive();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(b.IsClosed());
}

TEST(TCPKeepalive, PingsHoldAnIdleConnectionOpen) {
  ASSERT_EQ(Init(), Result::Success);
  SocketPair pair;
  ASSERT_TRUE(pair.ok);
  // a pings well inside b's idle window; neither ever sends data
  TCPTransportLayer a(pair.a, Timers(20, 0));
  TCPTransportLayer b(pair.b, Timers(0, 150));

  Pump(a, b, 500);
  EXPECT_FALSE(b.IsClosed()) << "pings alone must feed the idle timer";
  EXPECT_FALSE(a.IsClosed());
}

TEST(TCPKeepalive, PongsAnswerSoThePingerSeesALivePeer) {
  ASSERT_EQ(Init(), Result::Success);
  SocketPair pair;
  ASSERT_TRUE(pair.ok);
  // only a pings; the only traffic a can ever receive is b's pongs
  TCPTransportLayer a(pair.a, Timers(20, 150));
  TCPTransportLayer b(pair.b, Timers(0, 0));

  Pump(a, b, 500);
  EXPECT_FALSE(a.IsClosed()) << "b's pongs must reach a inside its idle window";
}

TEST(TCPKeepalive, ZeroDisablesBothTimers) {
  ASSERT_EQ(Init(), Result::Success);
  SocketPair pair;
  ASSERT_TRUE(pair.ok);
  TCPTransportLayer a(pair.a, Timers(0, 0));
  TCPTransportLayer b(pair.b, Timers(0, 0));

  Pump(a, b, 200);
  EXPECT_FALSE(a.IsClosed());
  EXPECT_FALSE(b.IsClosed());
}

// --- Round-trip latency -------------------------------------------------------

namespace {

enum EchoPacketType : PacketId { kPacketEcho = 1 };

class EchoPacket : public Packet {
 public:
  EchoPacket() : Packet(kPacketEcho) {}
  uint32_t seq = 0;
};

class EchoSerializer : public PacketSerializer<EchoPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<EchoPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->seq);
    return buffer;
  }
  std::shared_ptr<EchoPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<EchoPacket>();
    packet->seq = buffer->ReadInt<uint32_t>();
    return packet;
  }
};

std::shared_ptr<Codec> MakeEchoCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketEcho, std::make_unique<EchoSerializer>());
  return codec;
}

class EchoBack : public PacketHandler<EchoBack, EchoPacket> {
 public:
  explicit EchoBack(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}
  void OnPacket(std::shared_ptr<EchoPacket> packet) {
    session_->SendPacket(packet);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

class PongFlag : public PacketHandler<PongFlag, EchoPacket> {
 public:
  void OnPacket(std::shared_ptr<EchoPacket> packet) {
    (void)packet;
    got++;
  }
  std::atomic<int> got{0};
};

PortNumber FreeTcpPortLocal() {
  SocketHandle probe = socket(AF_INET, SOCK_STREAM, 0);
  PortNumber port = 0;
  auto any = InetAddress::from("127.0.0.1", 0);
  if (bind(probe, any->handle_ptr(), any->addr_size()) == 0) {
    sockaddr_storage addr{};
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    if (getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      auto bound = InetAddress::from(reinterpret_cast<sockaddr*>(&addr));
      if (bound) {
        port = bound->port();
      }
    }
  }
  CloseSocket(probe);
  return port;
}

}  // namespace

// the regression this pins: inbound TCP used to sit until a server worker's
// next 120 tps tick, an ~8 ms floor on every round trip. With the poll wake,
// a loopback ping-pong is microseconds.
TEST(TCPLatency, RoundTripBeatsTheOldTickFloor) {
  ASSERT_EQ(Init(), Result::Success);
  const PortNumber port = FreeTcpPortLocal();
  ASSERT_NE(port, 0);

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::TCP};
  Server server{server_config};
  server.SetEventCallback([](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [](IncomingClientConnectedEvent& ev) {
          ev.session()->SetCodec(MakeEchoCodec());
          ev.session()->SetHandler(std::make_shared<EchoBack>(ev.session()));
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::TCP};
  Client client{client_config};
  auto pong = std::make_shared<PongFlag>();
  std::atomic<bool> connected{false};
  std::shared_ptr<PeerSession> session;
  std::mutex session_mutex;
  client.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&](ClientConnectedToServerEvent& ev) {
          ev.session()->SetCodec(MakeEchoCodec());
          ev.session()->SetHandler(pong);
          {
            std::lock_guard<std::mutex> lock(session_mutex);
            session = ev.session();
          }
          connected = true;
          return false;
        });
  });
  ASSERT_EQ(client.Bind(), Result::Success);
  ASSERT_EQ(client.Connect(), Result::Success);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!connected.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(connected.load());
  std::shared_ptr<PeerSession> ping_session;
  {
    std::lock_guard<std::mutex> lock(session_mutex);
    ping_session = session;
  }

  // a warmup ping, then the measured ones
  std::vector<double> rtts_ms;
  for (uint32_t i = 0; i < 31; i++) {
    const int before = pong->got.load();
    auto packet = std::make_shared<EchoPacket>();
    packet->seq = i;
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(ping_session->SendPacket(packet), Result::Success);
    while (pong->got.load() == before &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ASSERT_GT(pong->got.load(), before) << "pong " << i << " never arrived";
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count() /
                      1000.0;
    if (i > 0) {
      rtts_ms.push_back(ms);
    }
  }
  std::sort(rtts_ms.begin(), rtts_ms.end());
  const double p50 = rtts_ms[rtts_ms.size() / 2];
  ZNET_LOG_INFO("TCP loopback round trip p50: {:.3f} ms", p50);
  EXPECT_LT(p50, 3.0) << "the old tick-polled floor was ~8.4 ms";

  client.Disconnect();
  server.Stop();
  client.Wait();
  server.Wait();
}

TEST(TCPKeepalive, DataSurvivesInterleavedControlFrames) {
  ASSERT_EQ(Init(), Result::Success);
  SocketPair pair;
  ASSERT_TRUE(pair.ok);
  // an aggressive ping cadence, so control frames land between data frames
  TCPTransportLayer a(pair.a, Timers(1, 0));
  TCPTransportLayer b(pair.b, Timers(1, 0));

  const uint32_t kMessages = 200;
  uint32_t received = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    ASSERT_TRUE(a.Send(payload));
    a.Update();
    b.Update();
    while (auto got = b.Receive()) {
      EXPECT_EQ(got->ReadInt<uint32_t>(), received);
      received++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (received < kMessages && std::chrono::steady_clock::now() < deadline) {
    a.Update();
    b.Update();
    while (auto got = b.Receive()) {
      EXPECT_EQ(got->ReadInt<uint32_t>(), received);
      received++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(received, kMessages)
      << "every data frame must arrive intact between the pings";
  EXPECT_FALSE(a.IsClosed());
  EXPECT_FALSE(b.IsClosed());
}
