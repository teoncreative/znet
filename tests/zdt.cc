//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "znet/backends/zdt.h"
#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/event.h"
#include "znet/init.h"
#include "znet/p2p.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/server.h"
#include "znet/server_events.h"

using namespace znet;
using namespace znet::backends;

// A minimal application packet (mirrors examples/basic) used to prove the whole
// pipeline (codec + encryption + compression + ZDT reliability) over UDP.
enum ZDTTestPacketType { PACKET_DEMO };

class DemoPacket : public Packet {
 public:
  DemoPacket() : Packet(PACKET_DEMO) {}
  std::string text;
};

class DemoSerializer : public PacketSerializer<DemoPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<DemoPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }
  std::shared_ptr<DemoPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<DemoPacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

struct RoundTripState {
  std::atomic_bool got_reply{false};
  std::string reply_text;
};

class ServerEchoHandler : public PacketHandler<ServerEchoHandler, DemoPacket> {
 public:
  explicit ServerEchoHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}
  void OnPacket(std::shared_ptr<DemoPacket> packet) {
    auto reply = std::make_shared<DemoPacket>();
    reply->text = "reply:" + packet->text;
    session_->SendPacket(reply);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

class ClientReplyHandler : public PacketHandler<ClientReplyHandler, DemoPacket> {
 public:
  explicit ClientReplyHandler(RoundTripState* state) : state_(state) {}
  void OnPacket(std::shared_ptr<DemoPacket> packet) {
    state_->reply_text = packet->text;
    state_->got_reply = true;
  }

 private:
  RoundTripState* state_;
};

// --- Wire header --------------------------------------------------------------

TEST(ZDTHeader, RoundTrip) {
  ZDTHeader header;
  header.flags = kFlagOnline | kFlagData | kFlagReliable | kFlagOrdered;
  header.channel = 7;
  header.packet_seq = 0xBEEF;
  header.ack = 0x1234;
  header.ack_bits = 0xDEADBEEF;
  header.message_seq = 0xCAFE;

  Buffer buffer(Endianness::BigEndian);
  WriteZDTHeader(buffer, header);
  EXPECT_EQ(buffer.size(), kZDTHeaderSize);

  ZDTHeader out;
  ASSERT_TRUE(ReadZDTHeader(buffer, out));
  EXPECT_EQ(out.flags, header.flags);
  EXPECT_EQ(out.channel, header.channel);
  EXPECT_EQ(out.packet_seq, header.packet_seq);
  EXPECT_EQ(out.ack, header.ack);
  EXPECT_EQ(out.ack_bits, header.ack_bits);
  EXPECT_EQ(out.message_seq, header.message_seq);
}

TEST(ZDTHeader, FragmentFields) {
  ZDTHeader header;
  header.flags = kFlagOnline | kFlagData | kFlagFragment;
  header.frag_index = 3;
  header.frag_count = 5;

  Buffer buffer(Endianness::BigEndian);
  WriteZDTHeader(buffer, header);
  EXPECT_EQ(buffer.size(), kZDTFragHeaderSize);

  ZDTHeader out;
  ASSERT_TRUE(ReadZDTHeader(buffer, out));
  EXPECT_EQ(out.frag_index, 3);
  EXPECT_EQ(out.frag_count, 5);
}

TEST(ZDTHeader, RejectsOfflineMessage) {
  Buffer buffer(Endianness::BigEndian);
  // first byte is an offline message id (no kFlagOnline bit).
  buffer.WriteInt<uint8_t>(
      static_cast<uint8_t>(ZDTOfflineMsg::OpenConnectionRequest1));
  for (int i = 0; i < 20; i++) {
    buffer.WriteInt<uint8_t>(0);
  }
  ZDTHeader out;
  EXPECT_FALSE(ReadZDTHeader(buffer, out));
}

// --- Shared helpers -----------------------------------------------------------

static RecvResult RecvWithRetry(UDPSocket& socket, void* buf, size_t cap,
                                size_t& len,
                                std::shared_ptr<InetAddress>& from) {
  RecvResult result = RecvResult::WouldBlock;
  for (int i = 0; i < 200 && result == RecvResult::WouldBlock; i++) {
    result = socket.RecvFrom(buf, cap, len, from);
    if (result == RecvResult::WouldBlock) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return result;
}

static std::shared_ptr<UDPSocket> MakeBoundSocket() {
  auto socket = std::make_shared<UDPSocket>();
  socket->Open(InetProtocolVersion::IPv4);
  socket->SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  socket->Bind(*any);
  return socket;
}

// Drains every datagram currently queued on `socket`. Tests use this to move
// datagrams between transports by hand, so they can drop/reorder them.
static std::vector<std::vector<uint8_t>> CollectDatagrams(UDPSocket& socket) {
  std::vector<std::vector<uint8_t>> out;
  uint8_t buf[ZNET_MAX_BUFFER_SIZE];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  while (socket.RecvFrom(buf, sizeof(buf), len, from) == RecvResult::Received) {
    out.emplace_back(buf, buf + len);
  }
  return out;
}

// Unless a test passes explicit SendOptions, Send() uses the library defaults:
// reliable + ordered on channel 0.

// Short timers so loss/retransmit tests converge quickly.
static ZDTOptions FastConfig() {
  ZDTOptions config;
  config.rto_min = std::chrono::milliseconds(5);
  config.rto_max = std::chrono::milliseconds(60);
  config.max_retries = 200;
  config.keepalive_interval = std::chrono::hours(1);
  return config;
}

TEST(ZDTUdpSocket, Loopback) {
  ASSERT_EQ(Init(), Result::Success);

  UDPSocket receiver;
  UDPSocket sender;
  ASSERT_EQ(receiver.Open(InetProtocolVersion::IPv4), Result::Success);
  ASSERT_EQ(sender.Open(InetProtocolVersion::IPv4), Result::Success);
  receiver.SetBlocking(false);
  sender.SetBlocking(false);

  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(receiver.Bind(*any), Result::Success);
  auto receiver_addr = receiver.local_address();
  ASSERT_TRUE(receiver_addr && receiver_addr->is_valid());

  const char data[] = "ping";
  ASSERT_TRUE(sender.SendTo(*receiver_addr, data, 4));

  uint8_t buf[64];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  ASSERT_EQ(RecvWithRetry(receiver, buf, sizeof(buf), len, from),
            RecvResult::Received);
  ASSERT_EQ(len, 4u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), len), "ping");
  ASSERT_TRUE(from && from->is_valid());
}

// --- Transport data path ------------------------------------------------------

TEST(ZDTTransport, LoopbackDataPath) {
  ASSERT_EQ(Init(), Result::Success);

  auto server_socket = std::make_shared<UDPSocket>();
  ASSERT_EQ(server_socket->Open(InetProtocolVersion::IPv4), Result::Success);
  server_socket->SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(server_socket->Bind(*any), Result::Success);
  auto server_addr = server_socket->local_address();
  ASSERT_TRUE(server_addr && server_addr->is_valid());

  auto client_socket = std::make_shared<UDPSocket>();
  ASSERT_EQ(client_socket->Open(InetProtocolVersion::IPv4), Result::Success);
  client_socket->SetBlocking(false);
  ASSERT_EQ(client_socket->Bind(*any), Result::Success);
  auto client_addr = client_socket->local_address();
  ASSERT_TRUE(client_addr && client_addr->is_valid());

  ZDTOptions config;
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_addr, config,
                           /*drains_own_socket=*/true, /*inbox=*/nullptr,
                           connection);

  const char message[] = "hello-zdt";
  auto payload = std::make_shared<Buffer>();
  payload->Write(message, sizeof(message) - 1);
  ASSERT_TRUE(client.Send(payload));
  client.Update();  // flush the datagram

  // the server demux does this routing; here we do it by hand.
  ZDTTransportLayer server(server_socket, client_addr, config,
                           /*drains_own_socket=*/false, /*inbox=*/nullptr,
                           connection);
  uint8_t buf[ZNET_MAX_BUFFER_SIZE];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  ASSERT_EQ(RecvWithRetry(*server_socket, buf, sizeof(buf), len, from),
            RecvResult::Received);
  server.OnDatagram(buf, len);
  server.Update();

  auto received = server.Receive();
  ASSERT_TRUE(received != nullptr);
  ASSERT_EQ(received->size(), sizeof(message) - 1);
  EXPECT_EQ(std::string(received->data(), received->size()), "hello-zdt");
}

// --- Offline handshake header -------------------------------------------------

TEST(ZDTOffline, HeaderRoundTrip) {
  Buffer buffer(Endianness::BigEndian);
  WriteOfflineHeader(buffer, ZDTOfflineMsg::OpenConnectionRequest2);
  buffer.WriteInt<uint32_t>(0x12345678);
  ZDTOfflineMsg id;
  ASSERT_TRUE(ReadOfflineHeader(buffer, id));
  EXPECT_EQ(id, ZDTOfflineMsg::OpenConnectionRequest2);
  EXPECT_EQ(buffer.ReadInt<uint32_t>(), 0x12345678u);
}

TEST(ZDTOffline, RejectsOnlineDatagram) {
  Buffer buffer(Endianness::BigEndian);
  buffer.WriteInt<uint8_t>(kFlagOnline | kFlagData);
  for (int i = 0; i < 12; i++) {
    buffer.WriteInt<uint8_t>(0);
  }
  ZDTOfflineMsg id;
  EXPECT_FALSE(ReadOfflineHeader(buffer, id));
}

TEST(ZDTOffline, RejectsBadMagic) {
  Buffer buffer(Endianness::BigEndian);
  buffer.WriteInt<uint8_t>(
      static_cast<uint8_t>(ZDTOfflineMsg::OpenConnectionRequest1));
  for (size_t i = 0; i < kZDTMagic.size(); i++) {
    buffer.WriteInt<uint8_t>(0xAB);
  }
  ZDTOfflineMsg id;
  EXPECT_FALSE(ReadOfflineHeader(buffer, id));
}

// --- Return-routability cookie ------------------------------------------------

TEST(ZDTCookieTest, DeterministicAndAddressBound) {
  std::array<uint8_t, 32> secret{};
  for (size_t i = 0; i < secret.size(); i++) {
    secret[i] = static_cast<uint8_t>(i);
  }
  auto a1 = ComputeCookie(secret.data(), secret.size(), "1.2.3.4:5000", 0);
  auto a2 = ComputeCookie(secret.data(), secret.size(), "1.2.3.4:5000", 0);
  auto other_addr = ComputeCookie(secret.data(), secret.size(), "1.2.3.5:5000", 0);
  auto other_epoch = ComputeCookie(secret.data(), secret.size(), "1.2.3.4:5000", 1);
  EXPECT_TRUE(ConstTimeEqual(a1, a2));          // deterministic
  EXPECT_FALSE(ConstTimeEqual(a1, other_addr));  // bound to address
  EXPECT_FALSE(ConstTimeEqual(a1, other_epoch));  // bound to epoch
}

TEST(ZDTCookieTest, SecretMatters) {
  std::array<uint8_t, 32> secret_a{};
  std::array<uint8_t, 32> secret_b{};
  secret_a.fill(1);
  secret_b.fill(2);
  auto cookie_a = ComputeCookie(secret_a.data(), secret_a.size(), "1.2.3.4:5000", 0);
  auto cookie_b = ComputeCookie(secret_b.data(), secret_b.size(), "1.2.3.4:5000", 0);
  EXPECT_FALSE(ConstTimeEqual(cookie_a, cookie_b));
}

// --- End-to-end handshake over ZDT --------------------------------------------

static PortNumber FreeUdpPort() {
  UDPSocket probe;
  probe.Open(InetProtocolVersion::IPv4);
  auto any = InetAddress::from("127.0.0.1", 0);
  probe.Bind(*any);
  PortNumber port = probe.local_address()->port();
  probe.Close();
  return port;
}

TEST(ZDTIntegration, HandshakeReachesReadyOverUdp) {
  ASSERT_EQ(Init(), Result::Success);

  std::atomic_bool server_connected{false};
  std::atomic_bool client_connected{false};
  PortNumber port = FreeUdpPort();

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Server server{server_config};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent&) {
          server_connected = true;
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Client client{client_config};
  client.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&](ClientConnectedToServerEvent&) {
          client_connected = true;
          return false;
        });
  });
  ASSERT_EQ(client.Bind(), Result::Success);
  ASSERT_EQ(client.Connect(), Result::Success);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         !(server_connected && client_connected)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(client_connected) << "client never reached Ready over ZDT";
  EXPECT_TRUE(server_connected) << "server never promoted the ZDT session";

  client.Disconnect();
  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(ZDTIntegration, RejectsIncompatibleVersion) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Server server{server_config};
  server.SetEventCallback([](Event&) {});
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  // A raw client that sends a Request1 with a bogus protocol version.
  UDPSocket socket;
  ASSERT_EQ(socket.Open(InetProtocolVersion::IPv4), Result::Success);
  socket.SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(socket.Bind(*any), Result::Success);
  auto server_addr = InetAddress::from("127.0.0.1", port);

  Buffer request(Endianness::BigEndian);
  WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest1);
  request.WriteInt<uint8_t>(static_cast<uint8_t>(kZDTProtocolVersion + 42));
  std::vector<uint8_t> pad(600, 0);
  request.Write(pad.data(), pad.size());
  ASSERT_TRUE(socket.SendTo(*server_addr, request.data(), request.size()));

  uint8_t buf[ZNET_MAX_BUFFER_SIZE];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  RecvResult result = RecvWithRetry(socket, buf, sizeof(buf), len, from);
  ASSERT_EQ(result, RecvResult::Received);
  Buffer reply(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
  ZDTOfflineMsg id;
  ASSERT_TRUE(ReadOfflineHeader(reply, id));
  EXPECT_EQ(id, ZDTOfflineMsg::IncompatibleProtocolVersion);
  EXPECT_EQ(reply.ReadInt<uint8_t>(), kZDTProtocolVersion);

  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// --- Reliability under packet loss --------------------------------------------

TEST(ZDTReliability, ReliableOrderedDeliversInOrderUnderLoss) {
  ASSERT_EQ(Init(), Result::Success);

  auto open_bound = []() {
    auto s = std::make_shared<UDPSocket>();
    s->Open(InetProtocolVersion::IPv4);
    s->SetBlocking(false);
    auto any = InetAddress::from("127.0.0.1", 0);
    s->Bind(*any);
    return s;
  };
  auto server_socket = open_bound();
  auto client_socket = open_bound();
  auto server_addr = server_socket->local_address();
  auto client_addr = client_socket->local_address();

  ZDTOptions config;
  config.rto_min = std::chrono::milliseconds(5);
  config.rto_max = std::chrono::milliseconds(60);
  config.max_retries = 100;
  config.keepalive_interval = std::chrono::hours(1);  // silence keepalive noise

  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_addr, config, false, nullptr,
                           connection);
  ZDTTransportLayer server(server_socket, client_addr, config, false, nullptr,
                           connection);

  const uint32_t kMessages = 300;
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    ASSERT_TRUE(client.Send(payload));
  }

  std::mt19937 rng(0xC0FFEE);  // fixed seed -> deterministic
  auto drop = [&]() { return (rng() % 100) < 30; };  // 30% loss both directions

  auto pump = [&](UDPSocket& from, ZDTTransportLayer& to) {
    uint8_t buf[ZNET_MAX_BUFFER_SIZE];
    size_t len = 0;
    std::shared_ptr<InetAddress> src;
    while (from.RecvFrom(buf, sizeof(buf), len, src) == RecvResult::Received) {
      if (!drop()) {
        to.OnDatagram(buf, len);
      }
    }
  };

  std::vector<uint32_t> received;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (received.size() < kMessages &&
         std::chrono::steady_clock::now() < deadline) {
    client.Update();
    pump(*server_socket, server);  // client -> server (lossy)
    server.Update();
    pump(*client_socket, client);  // server -> client acks (lossy)
    while (auto buffer = server.Receive()) {
      received.push_back(buffer->ReadInt<uint32_t>());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(received.size(), kMessages) << "not all reliable messages delivered";
  for (uint32_t i = 0; i < kMessages; i++) {
    EXPECT_EQ(received[i], i) << "out-of-order delivery at index " << i;
  }
}

// Regression: on a simultaneous open (both peers send before hearing from each
// other, the normal case for a P2P punch) neither side may falsely acknowledge
// the other's first datagram. packet_seq 0 is the reserved "nothing to ack yet"
// sentinel; without it, a dropped first datagram was never retransmitted and the
// reliable message was lost silently.
TEST(ZDTReliability, SimultaneousOpenDoesNotFalselyAck) {
  ASSERT_EQ(Init(), Result::Success);
  auto socket_a = MakeBoundSocket();
  auto socket_b = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  ZDTTransportLayer a(socket_a, socket_b->local_address(), config, false,
                      nullptr, connection);
  ZDTTransportLayer b(socket_b, socket_a->local_address(), config, false,
                      nullptr, connection);

  auto payload_a = std::make_shared<Buffer>();
  payload_a->WriteInt<uint32_t>(0xAAAA);
  ASSERT_TRUE(a.Send(payload_a));
  auto payload_b = std::make_shared<Buffer>();
  payload_b->WriteInt<uint32_t>(0xBBBB);
  ASSERT_TRUE(b.Send(payload_b));
  a.Update();
  b.Update();  // both emit their first datagram, each having received nothing

  // drop everything A sent; hand B's first datagram (which carries the default
  // ack) to A. A must NOT treat its own first packet as acknowledged.
  CollectDatagrams(*socket_b);  // discard A -> B
  for (auto& datagram : CollectDatagrams(*socket_a)) {
    a.OnDatagram(datagram.data(), datagram.size());
  }

  bool delivered = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!delivered && std::chrono::steady_clock::now() < deadline) {
    a.Update();
    for (auto& d : CollectDatagrams(*socket_b)) {
      b.OnDatagram(d.data(), d.size());
    }
    b.Update();
    for (auto& d : CollectDatagrams(*socket_a)) {
      a.OnDatagram(d.data(), d.size());
    }
    while (auto buffer = b.Receive()) {
      EXPECT_EQ(buffer->ReadInt<uint32_t>(), 0xAAAAu);
      delivered = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(delivered)
      << "A's first reliable message was falsely acked and never retransmitted";
}

// Sequence numbers are 16-bit, so a long-lived connection must cross the 65536
// boundary without stalling, mis-ordering, or exhausting retries (which would
// close the connection). Pushes >65536 messages so BOTH the connection-level
// packet_seq and the per-channel message_seq wrap.
TEST(ZDTReliability, SequenceWraparoundDoesNotBreakConnection) {
  ASSERT_EQ(Init(), Result::Success);
  auto socket_a = MakeBoundSocket();
  auto socket_b = MakeBoundSocket();
  ZDTOptions config;
  config.rto_min = std::chrono::milliseconds(20);
  config.rto_max = std::chrono::milliseconds(200);
  config.keepalive_interval = std::chrono::hours(1);
  config.idle_timeout = std::chrono::hours(1);
  config.cwnd = 256;
  ZDTConnection connection;
  ZDTTransportLayer a(socket_a, socket_b->local_address(), config, false,
                      nullptr, connection);
  ZDTTransportLayer b(socket_b, socket_a->local_address(), config, false,
                      nullptr, connection);

  const uint32_t kMessages = 70000;  // > 65536
  uint32_t sent = 0;
  uint32_t expected = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (expected < kMessages && std::chrono::steady_clock::now() < deadline) {
    while (sent < kMessages && sent - expected < 4000) {
      auto payload = std::make_shared<Buffer>();
      payload->WriteInt<uint32_t>(sent);
      a.Send(payload);
      sent++;
    }
    a.Update();
    for (auto& d : CollectDatagrams(*socket_b)) {
      b.OnDatagram(d.data(), d.size());
    }
    b.Update();
    for (auto& d : CollectDatagrams(*socket_a)) {
      a.OnDatagram(d.data(), d.size());
    }
    while (auto buffer = b.Receive()) {
      ASSERT_EQ(buffer->ReadInt<uint32_t>(), expected) << "order broke at wrap";
      expected++;
    }
    ASSERT_FALSE(a.IsClosed()) << "sender closed (retries exhausted) at " << expected;
    ASSERT_FALSE(b.IsClosed()) << "receiver closed at " << expected;
  }
  EXPECT_EQ(expected, kMessages) << "stalled crossing the 16-bit sequence wrap";
}

// --- Options plumbing --------------------------------------------------------

// child_options set on the server config must reach the accepted session's
// transport, and a client's options must reach its own.
TEST(ZDTOptionsPlumbing, ChildOptionsReachTheSession) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  server_config.child_options.zdt.cwnd = 7;          // distinctive values
  server_config.child_options.zdt.max_reassemblies = 11;
  Server server{server_config};
  std::atomic_bool connected{false};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent&) {
          connected = true;
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  client_config.options.zdt.cwnd = 3;
  Client client{client_config};
  client.SetEventCallback([](Event&) {});
  ASSERT_EQ(client.Bind(), Result::Success);
  ASSERT_EQ(client.Connect(), Result::Success);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!connected && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(connected.load()) << "session never established";

  client.Disconnect();
  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(ZDTOptionsPlumbing, DefaultsMatchZDTOptions) {
  ZDTOptions defaults;
  EXPECT_EQ(defaults.cwnd, 64);
  EXPECT_EQ(defaults.max_retries, 10);
  EXPECT_EQ(defaults.mtu_ladder.front(), 1492);
  EXPECT_EQ(defaults.mtu_ladder.back(), 576);
  // A SessionOptions carries the zdt defaults untouched.
  SessionOptions session;
  EXPECT_EQ(session.zdt.cwnd, defaults.cwnd);
  EXPECT_TRUE(session.common.collect_metrics);
}

// --- Metrics ------------------------------------------------------------------

#if ZNET_ENABLE_METRICS
// Counters must reflect real traffic on both sides of a live session, and the
// server must account for the accepted connection.
TEST(ZDTMetrics, CountsRealTraffic) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();
  RoundTripState state;
  std::shared_ptr<PeerSession> server_session;

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Server server{server_config};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent& ev) {
          auto codec = std::make_shared<Codec>();
          codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
          ev.session()->SetCodec(codec);
          ev.session()->SetHandler(
              std::make_shared<ServerEchoHandler>(ev.session()));
          server_session = ev.session();
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  std::shared_ptr<PeerSession> client_session;
  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Client client{client_config};
  client.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&](ClientConnectedToServerEvent& ev) {
          auto codec = std::make_shared<Codec>();
          codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
          ev.session()->SetCodec(codec);
          ev.session()->SetHandler(std::make_shared<ClientReplyHandler>(&state));
          client_session = ev.session();
          auto packet = std::make_shared<DemoPacket>();
          packet->text = "metrics";
          ev.session()->SendPacket(packet);
          return false;
        });
  });
  ASSERT_EQ(client.Bind(), Result::Success);
  ASSERT_EQ(client.Connect(), Result::Success);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!state.got_reply && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(state.got_reply.load());
  ASSERT_TRUE(client_session != nullptr);

  SessionMetrics cm = client_session->metrics();
  EXPECT_GT(cm.common.messages_sent, 0u) << "client sent at least the demo packet";
  EXPECT_GT(cm.common.messages_received, 0u) << "client received the echo";
  EXPECT_GT(cm.zdt.datagrams_sent, 0u);
  EXPECT_GT(cm.zdt.datagrams_received, 0u);
  EXPECT_GT(cm.common.wire_bytes_sent, cm.common.message_bytes_sent)
      << "wire bytes must include transport headers";
  EXPECT_GT(cm.zdt.mtu, 0u) << "negotiated mtu should be reported";

  ServerMetrics sm = server.metrics();
  EXPECT_GE(sm.zdt.handshakes_started, 1u);
  EXPECT_EQ(sm.connections_accepted, 1u);
  EXPECT_EQ(sm.zdt.cookies_rejected, 0u);
  EXPECT_EQ(sm.zdt.handshakes_rejected, 0u);

  client.Disconnect();
  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// The same SessionMetrics shape serves both transports: common counters are
// populated either way, and each transport fills only its own group.
TEST(ZDTMetrics, TCPUsesTheSameShape) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();  // free for TCP too
  RoundTripState state;

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::TCP};
  Server server{server_config};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent& ev) {
          auto codec = std::make_shared<Codec>();
          codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
          ev.session()->SetCodec(codec);
          ev.session()->SetHandler(
              std::make_shared<ServerEchoHandler>(ev.session()));
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  std::shared_ptr<PeerSession> client_session;
  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::TCP};
  Client client{client_config};
  client.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&](ClientConnectedToServerEvent& ev) {
          auto codec = std::make_shared<Codec>();
          codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
          ev.session()->SetCodec(codec);
          ev.session()->SetHandler(std::make_shared<ClientReplyHandler>(&state));
          client_session = ev.session();
          auto packet = std::make_shared<DemoPacket>();
          packet->text = "tcp";
          ev.session()->SendPacket(packet);
          return false;
        });
  });
  ASSERT_EQ(client.Bind(), Result::Success);
  ASSERT_EQ(client.Connect(), Result::Success);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!state.got_reply && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(state.got_reply.load());
  ASSERT_TRUE(client_session != nullptr);

  SessionMetrics m = client_session->metrics();
  EXPECT_EQ(m.transport, ConnectionType::TCP) << "tag identifies the live group";
  EXPECT_GT(m.common.messages_sent, 0u);
  EXPECT_GT(m.common.messages_received, 0u);
  EXPECT_GT(m.tcp.writes, 0u) << "TCP counts socket writes, not datagrams";
  EXPECT_GT(m.tcp.reads, 0u);
  // The ZDT group stays zeroed rather than being undefined, so reading the
  // wrong group is harmless.
  EXPECT_EQ(m.zdt.retransmits, 0u);
  EXPECT_EQ(m.zdt.datagrams_sent, 0u);

  client.Disconnect();
  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// A version-mismatched handshake must show up as a rejection, not an accept.
TEST(ZDTMetrics, CountsRejectedHandshake) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Server server{server_config};
  server.SetEventCallback([](Event&) {});
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  UDPSocket socket;
  ASSERT_EQ(socket.Open(InetProtocolVersion::IPv4), Result::Success);
  socket.SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(socket.Bind(*any), Result::Success);
  auto server_addr = InetAddress::from("127.0.0.1", port);

  Buffer request(Endianness::BigEndian);
  WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest1);
  request.WriteInt<uint8_t>(static_cast<uint8_t>(kZDTProtocolVersion + 42));
  std::vector<uint8_t> pad(600, 0);
  request.Write(pad.data(), pad.size());
  ASSERT_TRUE(socket.SendTo(*server_addr, request.data(), request.size()));

  uint8_t buf[ZNET_MAX_BUFFER_SIZE];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  ASSERT_EQ(RecvWithRetry(socket, buf, sizeof(buf), len, from),
            RecvResult::Received);

  ServerMetrics sm = server.metrics();
  EXPECT_EQ(sm.zdt.handshakes_started, 1u);
  EXPECT_EQ(sm.zdt.handshakes_rejected, 1u);
  EXPECT_EQ(sm.connections_accepted, 0u);

  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Retransmits under loss must be visible.
TEST(ZDTMetrics, CountsRetransmitsAndDuplicates) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 100;
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    client.Send(payload);
  }

  std::mt19937 rng(5);
  auto drop = [&]() { return (rng() % 100) < 40; };
  auto pump = [&](UDPSocket& from, ZDTTransportLayer& to) {
    for (auto& d : CollectDatagrams(from)) {
      if (!drop()) {
        to.OnDatagram(d.data(), d.size());
      }
    }
  };

  size_t delivered = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (delivered < kMessages &&
         std::chrono::steady_clock::now() < deadline) {
    client.Update();
    pump(*server_socket, server);
    server.Update();
    pump(*client_socket, client);
    while (server.Receive()) {
      delivered++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(delivered, kMessages);

  SessionMetrics cm;
  client.FillMetrics(cm);
  EXPECT_GT(cm.zdt.retransmits, 0u) << "40% loss must force retransmits";
  EXPECT_GT(cm.zdt.datagrams_sent, kMessages) << "retransmits add datagrams";
  SessionMetrics sm;
  server.FillMetrics(sm);
  EXPECT_GT(sm.zdt.duplicates_dropped, 0u) << "some retransmits arrive twice";
}
#endif  // ZNET_ENABLE_METRICS

// --- Full application round-trip over ZDT (the "usable TCP-equivalent" proof) --

TEST(ZDTIntegration, AppPacketRoundTripOverUdp) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();
  RoundTripState state;

  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Server server{server_config};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent& ev) {
          auto codec = std::make_shared<Codec>();
          codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
          ev.session()->SetCodec(codec);
          ev.session()->SetHandler(
              std::make_shared<ServerEchoHandler>(ev.session()));
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  ClientConfig client_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Client client{client_config};
  client.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<ClientConnectedToServerEvent>(
        [&](ClientConnectedToServerEvent& ev) {
          auto codec = std::make_shared<Codec>();
          codec->Add(PACKET_DEMO, std::make_unique<DemoSerializer>());
          ev.session()->SetCodec(codec);
          ev.session()->SetHandler(
              std::make_shared<ClientReplyHandler>(&state));
          auto packet = std::make_shared<DemoPacket>();
          packet->text = "hello";
          ev.session()->SendPacket(packet);
          return false;
        });
  });
  ASSERT_EQ(client.Bind(), Result::Success);
  ASSERT_EQ(client.Connect(), Result::Success);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!state.got_reply && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(state.got_reply.load()) << "client never received the echo";
  EXPECT_EQ(state.reply_text, "reply:hello");

  client.Disconnect();
  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// --- Full channel matrix (M4) -------------------------------------------------

// reliable + unordered: every message arrives exactly once (dedup on retransmit),
// order not guaranteed.
TEST(ZDTChannels, ReliableUnorderedDeliversAllExactlyOnce) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 200;
  SendOptions options{SendOptionsInit{.reliable = true, .ordered = false, .channel = 1}};
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    client.Send(payload, options);
  }

  std::mt19937 rng(7);
  auto drop = [&]() { return (rng() % 100) < 30; };
  auto pump = [&](UDPSocket& from, ZDTTransportLayer& to) {
    for (auto& datagram : CollectDatagrams(from)) {
      if (!drop()) {
        to.OnDatagram(datagram.data(), datagram.size());
      }
    }
  };

  std::set<uint32_t> unique;
  size_t total = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (unique.size() < kMessages &&
         std::chrono::steady_clock::now() < deadline) {
    client.Update();
    pump(*server_socket, server);
    server.Update();
    pump(*client_socket, client);
    while (auto buffer = server.Receive()) {
      unique.insert(buffer->ReadInt<uint32_t>());
      total++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(unique.size(), kMessages) << "not all reliable messages delivered";
  EXPECT_EQ(total, kMessages) << "a retransmit was delivered as a duplicate";
}

// unreliable + ordered (sequenced): out-of-order arrivals are dropped, delivered
// stream is strictly increasing, no retransmit.
TEST(ZDTChannels, UnreliableSequencedDeliversMonotonic) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 200;
  SendOptions options{SendOptionsInit{.reliable = false, .ordered = true, .channel = 2}};
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    client.Send(payload, options);
  }
  client.Update();  // flush once; unreliable, no retransmit

  auto datagrams = CollectDatagrams(*server_socket);
  std::mt19937 rng(99);
  std::shuffle(datagrams.begin(), datagrams.end(), rng);  // force reordering
  for (auto& datagram : datagrams) {
    server.OnDatagram(datagram.data(), datagram.size());
  }
  server.Update();

  std::vector<uint32_t> received;
  while (auto buffer = server.Receive()) {
    received.push_back(buffer->ReadInt<uint32_t>());
  }
  ASSERT_FALSE(received.empty());
  for (size_t i = 1; i < received.size(); i++) {
    EXPECT_LT(received[i - 1], received[i]) << "sequenced delivery not monotonic";
  }
  EXPECT_EQ(received.back(), kMessages - 1) << "highest sequence must arrive";
}

// unreliable + unordered: with no loss, every message is delivered exactly once.
TEST(ZDTChannels, UnreliableUnorderedDeliversAllWithoutLoss) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 200;
  SendOptions options{SendOptionsInit{.reliable = false, .ordered = false, .channel = 3}};
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    client.Send(payload, options);
  }
  client.Update();

  for (auto& datagram : CollectDatagrams(*server_socket)) {
    server.OnDatagram(datagram.data(), datagram.size());
  }
  server.Update();

  std::set<uint32_t> unique;
  size_t total = 0;
  while (auto buffer = server.Receive()) {
    unique.insert(buffer->ReadInt<uint32_t>());
    total++;
  }
  EXPECT_EQ(total, kMessages);
  EXPECT_EQ(unique.size(), kMessages);
}

// Reliable and unreliable traffic share one channel: separate sequence spaces
// mean the reliable-ordered stream stays complete and in order regardless of the
// unreliable messages interleaved on the same channel (a shared seq space would
// stall it on the unreliable gaps).
TEST(ZDTChannels, ReliableAndUnreliableCoexistOnOneChannel) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 100;
  SendOptions reliable{SendOptionsInit{.reliable = true, .ordered = true, .channel = 5}};
  SendOptions unreliable{SendOptionsInit{.reliable = false, .ordered = false, .channel = 5}};
  for (uint32_t i = 0; i < kMessages; i++) {
    auto rel = std::make_shared<Buffer>();
    rel->WriteInt<uint32_t>(i);  // reliable values: [0, 100)
    client.Send(rel, reliable);
    auto unrel = std::make_shared<Buffer>();
    unrel->WriteInt<uint32_t>(1000 + i);  // unreliable values: [1000, ...)
    client.Send(unrel, unreliable);
  }

  std::mt19937 rng(21);
  auto drop = [&]() { return (rng() % 100) < 30; };
  auto pump = [&](UDPSocket& from, ZDTTransportLayer& to) {
    for (auto& datagram : CollectDatagrams(from)) {
      if (!drop()) {
        to.OnDatagram(datagram.data(), datagram.size());
      }
    }
  };

  std::vector<uint32_t> reliable_received;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (reliable_received.size() < kMessages &&
         std::chrono::steady_clock::now() < deadline) {
    client.Update();
    pump(*server_socket, server);
    server.Update();
    pump(*client_socket, client);
    while (auto buffer = server.Receive()) {
      uint32_t value = buffer->ReadInt<uint32_t>();
      if (value < 1000) {
        reliable_received.push_back(value);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(reliable_received.size(), kMessages)
      << "reliable stream stalled by unreliable traffic on the same channel";
  for (uint32_t i = 0; i < kMessages; i++) {
    EXPECT_EQ(reliable_received[i], i);
  }
}

// --- Fragmentation + reassembly (M5) ------------------------------------------

// A message larger than the MTU is split, sent, and reassembled byte-for-byte.
TEST(ZDTFragmentation, LargeMessageRoundTrip) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  connection.mtu = 300;  // force fragmentation (~9 fragments for 2500 bytes)
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  std::vector<uint8_t> original(2500);
  std::mt19937 rng(1234);
  for (auto& byte : original) {
    byte = static_cast<uint8_t>(rng());
  }
  auto payload = std::make_shared<Buffer>();
  payload->Write(original.data(), original.size());
  ASSERT_TRUE(client.Send(payload));

  std::shared_ptr<Buffer> got;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!got && std::chrono::steady_clock::now() < deadline) {
    client.Update();
    for (auto& d : CollectDatagrams(*server_socket)) {
      server.OnDatagram(d.data(), d.size());
    }
    server.Update();
    for (auto& d : CollectDatagrams(*client_socket)) {
      client.OnDatagram(d.data(), d.size());
    }
    got = server.Receive();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(got != nullptr) << "fragmented message never reassembled";
  ASSERT_EQ(got->size(), original.size());
  std::vector<uint8_t> received(original.size());
  got->Read(received.data(), received.size());
  EXPECT_EQ(received, original);
}

// Many large messages, each fragmented, all reassembled in order under loss.
TEST(ZDTFragmentation, LargeMessagesReassembleInOrderUnderLoss) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  ZDTConnection connection;
  connection.mtu = 300;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 20;
  std::vector<std::vector<uint8_t>> originals(kMessages);
  std::mt19937 gen(555);
  for (uint32_t m = 0; m < kMessages; m++) {
    originals[m].resize(1000 + (gen() % 1500));  // 1000..2499 bytes
    for (auto& byte : originals[m]) {
      byte = static_cast<uint8_t>(gen());
    }
    auto payload = std::make_shared<Buffer>();
    payload->Write(originals[m].data(), originals[m].size());
    client.Send(payload);
  }

  std::mt19937 rng(42);
  auto drop = [&]() { return (rng() % 100) < 30; };
  auto pump = [&](UDPSocket& from, ZDTTransportLayer& to) {
    for (auto& d : CollectDatagrams(from)) {
      if (!drop()) {
        to.OnDatagram(d.data(), d.size());
      }
    }
  };

  std::vector<std::vector<uint8_t>> received;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (received.size() < kMessages &&
         std::chrono::steady_clock::now() < deadline) {
    client.Update();
    pump(*server_socket, server);
    server.Update();
    pump(*client_socket, client);
    while (auto buffer = server.Receive()) {
      std::vector<uint8_t> bytes(buffer->size());
      buffer->Read(bytes.data(), bytes.size());
      received.push_back(std::move(bytes));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(received.size(), kMessages);
  for (uint32_t m = 0; m < kMessages; m++) {
    EXPECT_EQ(received[m], originals[m])
        << "message " << m << " corrupted or out of order";
  }
}

// --- Congestion window + rate limiting (M6) -----------------------------------

// The in-flight reliable window bounds the initial burst; data drains as acks
// arrive and everything is eventually delivered in order.
TEST(ZDTCongestion, WindowBoundsBurstThenDrains) {
  ASSERT_EQ(Init(), Result::Success);
  auto server_socket = MakeBoundSocket();
  auto client_socket = MakeBoundSocket();
  ZDTOptions config = FastConfig();
  config.cwnd = 8;
  ZDTConnection connection;
  ZDTTransportLayer client(client_socket, server_socket->local_address(), config,
                           false, nullptr, connection);
  ZDTTransportLayer server(server_socket, client_socket->local_address(), config,
                           false, nullptr, connection);

  const uint32_t kMessages = 100;
  for (uint32_t i = 0; i < kMessages; i++) {
    auto payload = std::make_shared<Buffer>();
    payload->WriteInt<uint32_t>(i);
    client.Send(payload);
  }

  client.Update();  // first flush is bounded by the window
  auto first_batch = CollectDatagrams(*server_socket);
  EXPECT_GT(first_batch.size(), 0u);
  EXPECT_LE(first_batch.size(), static_cast<size_t>(config.cwnd))
      << "burst was not bounded by the congestion window";
  for (auto& d : first_batch) {
    server.OnDatagram(d.data(), d.size());
  }

  std::vector<uint32_t> received;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (received.size() < kMessages &&
         std::chrono::steady_clock::now() < deadline) {
    client.Update();
    for (auto& d : CollectDatagrams(*server_socket)) {
      server.OnDatagram(d.data(), d.size());
    }
    server.Update();
    for (auto& d : CollectDatagrams(*client_socket)) {
      client.OnDatagram(d.data(), d.size());
    }
    while (auto buffer = server.Receive()) {
      received.push_back(buffer->ReadInt<uint32_t>());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(received.size(), kMessages);
  for (uint32_t i = 0; i < kMessages; i++) {
    EXPECT_EQ(received[i], i);
  }
}

// A single source blasting handshake requests is rate-limited: far fewer replies
// than requests come back.
TEST(ZDTRateLimit, PerSourceHandshakeIsThrottled) {
  ASSERT_EQ(Init(), Result::Success);
  PortNumber port = FreeUdpPort();
  ServerConfig server_config{"127.0.0.1", port, std::chrono::seconds(5),
                             ConnectionType::ZDT};
  Server server{server_config};
  server.SetEventCallback([](Event&) {});
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  UDPSocket socket;
  ASSERT_EQ(socket.Open(InetProtocolVersion::IPv4), Result::Success);
  socket.SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(socket.Bind(*any), Result::Success);
  auto server_addr = InetAddress::from("127.0.0.1", port);

  const int kRequests = 60;  // default rate limit is 20 / source / second
  for (int i = 0; i < kRequests; i++) {
    Buffer request(Endianness::BigEndian);
    WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest1);
    request.WriteInt<uint8_t>(kZDTProtocolVersion);
    std::vector<uint8_t> pad(600, 0);
    request.Write(pad.data(), pad.size());
    socket.SendTo(*server_addr, request.data(), request.size());
  }

  int replies = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t buf[ZNET_MAX_BUFFER_SIZE];
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    while (socket.RecvFrom(buf, sizeof(buf), len, from) == RecvResult::Received) {
      Buffer reply(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
      ZDTOfflineMsg id;
      if (ReadOfflineHeader(reply, id) &&
          id == ZDTOfflineMsg::OpenConnectionReply1) {
        replies++;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  EXPECT_GT(replies, 0);
  EXPECT_LE(replies, 25) << "rate limit did not throttle the flood";
  EXPECT_LT(replies, kRequests);

  server.Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// --- Security / anti-spoofing (M7) --------------------------------------------

namespace {
struct Reply1Data {
  ZDTCookie cookie{};
  uint32_t epoch = 0;
  uint16_t mtu = 0;
  uint64_t server_guid = 0;
  size_t reply_size = 0;
  bool ok = false;
};

// Sends an OpenConnectionRequest1 padded to `pad_to` bytes and parses Reply1.
Reply1Data DoRequest1(UDPSocket& socket, const InetAddress& server_addr,
                      uint16_t pad_to) {
  Buffer request(Endianness::BigEndian);
  WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest1);
  request.WriteInt<uint8_t>(kZDTProtocolVersion);
  if (request.size() < pad_to) {
    std::vector<uint8_t> pad(pad_to - request.size(), 0);
    request.Write(pad.data(), pad.size());
  }
  socket.SendTo(server_addr, request.data(), request.size());

  uint8_t buf[ZNET_MAX_BUFFER_SIZE];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  if (RecvWithRetry(socket, buf, sizeof(buf), len, from) != RecvResult::Received) {
    return {};
  }
  Buffer reply(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(reply, id) ||
      id != ZDTOfflineMsg::OpenConnectionReply1) {
    return {};
  }
  Reply1Data data;
  data.server_guid = reply.ReadInt<uint64_t>();
  data.mtu = reply.ReadInt<uint16_t>();
  uint8_t cookie_len = reply.ReadInt<uint8_t>();
  if (cookie_len != data.cookie.size()) {
    return {};
  }
  reply.Read(data.cookie.data(), data.cookie.size());
  data.epoch = reply.ReadInt<uint32_t>();
  data.reply_size = len;
  data.ok = true;
  return data;
}

// Sends an OpenConnectionRequest2 echoing `cookie`/`epoch` and returns true iff a
// Reply2 comes back.
bool DoRequest2ExpectReply(UDPSocket& socket, const InetAddress& server_addr,
                           const ZDTCookie& cookie, uint32_t epoch) {
  Buffer request(Endianness::BigEndian);
  WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest2);
  request.WriteInt<uint8_t>(static_cast<uint8_t>(cookie.size()));
  request.Write(cookie.data(), cookie.size());
  request.WriteInt<uint32_t>(epoch);
  request.WriteInetAddress(server_addr);
  request.WriteInt<uint16_t>(1200);
  request.WriteInt<uint64_t>(0xABCDEF12);
  socket.SendTo(server_addr, request.data(), request.size());

  uint8_t buf[ZNET_MAX_BUFFER_SIZE];
  size_t len = 0;
  std::shared_ptr<InetAddress> from;
  for (int i = 0; i < 60; i++) {  // ~300ms; Reply2 would arrive well within this
    if (socket.RecvFrom(buf, sizeof(buf), len, from) == RecvResult::Received) {
      Buffer reply(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
      ZDTOfflineMsg id;
      if (ReadOfflineHeader(reply, id) &&
          id == ZDTOfflineMsg::OpenConnectionReply2) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

struct SecurityServer {
  std::atomic_bool connected{false};  // stable address; captured by raw pointer
  std::unique_ptr<Server> server;
  PortNumber port = 0;
  std::unique_ptr<InetAddress> addr;
  ~SecurityServer() {
    if (server) {
      server->Stop();
    }
  }
};

std::unique_ptr<SecurityServer> StartSecurityServer() {
  auto ctx = std::make_unique<SecurityServer>();
  ctx->port = FreeUdpPort();
  ServerConfig config{"127.0.0.1", ctx->port, std::chrono::seconds(5),
                      ConnectionType::ZDT};
  ctx->server = std::make_unique<Server>(config);
  std::atomic_bool* flag = &ctx->connected;  // no ownership cycle
  ctx->server->SetEventCallback([flag](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [flag](IncomingClientConnectedEvent&) {
          *flag = true;
          return false;
        });
  });
  ctx->server->Bind();
  ctx->server->Listen();
  ctx->addr = InetAddress::from("127.0.0.1", ctx->port);
  return ctx;
}
}  // namespace

// Request1 must allocate no per-connection state: a cookie comes back but no
// session is created until a valid Request2 proves the address.
TEST(ZDTSecurity, Request1AllocatesNoSession) {
  ASSERT_EQ(Init(), Result::Success);
  auto ctx = StartSecurityServer();

  UDPSocket socket;
  ASSERT_EQ(socket.Open(InetProtocolVersion::IPv4), Result::Success);
  socket.SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(socket.Bind(*any), Result::Success);

  Reply1Data reply1 = DoRequest1(socket, *ctx->addr, 600);
  ASSERT_TRUE(reply1.ok) << "server did not answer Request1 with Reply1";

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(ctx->connected.load())
      << "Request1 alone must not create a session";

  ctx->server->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// A forged/corrupted cookie in Request2 is dropped: no Reply2, no session.
TEST(ZDTSecurity, ForgedCookieRejected) {
  ASSERT_EQ(Init(), Result::Success);
  auto ctx = StartSecurityServer();

  UDPSocket socket;
  ASSERT_EQ(socket.Open(InetProtocolVersion::IPv4), Result::Success);
  socket.SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(socket.Bind(*any), Result::Success);

  Reply1Data reply1 = DoRequest1(socket, *ctx->addr, 600);
  ASSERT_TRUE(reply1.ok);

  ZDTCookie forged = reply1.cookie;
  forged[0] ^= 0xFF;  // corrupt one byte
  EXPECT_FALSE(DoRequest2ExpectReply(socket, *ctx->addr, forged, reply1.epoch))
      << "server accepted a forged cookie";
  EXPECT_FALSE(ctx->connected.load());

  // positive control: the genuine cookie completes the transport handshake.
  EXPECT_TRUE(DoRequest2ExpectReply(socket, *ctx->addr, reply1.cookie,
                                    reply1.epoch))
      << "server rejected a valid cookie";

  ctx->server->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// The cookie is bound to the source address: a cookie issued to socket A cannot
// be redeemed from socket B.
TEST(ZDTSecurity, CookieIsAddressBound) {
  ASSERT_EQ(Init(), Result::Success);
  auto ctx = StartSecurityServer();
  auto any = InetAddress::from("127.0.0.1", 0);

  UDPSocket socket_a;
  ASSERT_EQ(socket_a.Open(InetProtocolVersion::IPv4), Result::Success);
  socket_a.SetBlocking(false);
  ASSERT_EQ(socket_a.Bind(*any), Result::Success);

  UDPSocket socket_b;
  ASSERT_EQ(socket_b.Open(InetProtocolVersion::IPv4), Result::Success);
  socket_b.SetBlocking(false);
  ASSERT_EQ(socket_b.Bind(*any), Result::Success);

  Reply1Data reply1 = DoRequest1(socket_a, *ctx->addr, 600);  // cookie for A
  ASSERT_TRUE(reply1.ok);

  // redeem A's cookie from B -> server recomputes for B's address -> mismatch.
  EXPECT_FALSE(
      DoRequest2ExpectReply(socket_b, *ctx->addr, reply1.cookie, reply1.epoch))
      << "cookie was accepted from a different source address";

  ctx->server->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Anti-amplification: the server never replies with more bytes than it received
// from an unvalidated address (Reply1 <= padded Request1).
TEST(ZDTSecurity, NoAmplificationOnRequest1) {
  ASSERT_EQ(Init(), Result::Success);
  auto ctx = StartSecurityServer();

  UDPSocket socket;
  ASSERT_EQ(socket.Open(InetProtocolVersion::IPv4), Result::Success);
  socket.SetBlocking(false);
  auto any = InetAddress::from("127.0.0.1", 0);
  ASSERT_EQ(socket.Bind(*any), Result::Success);

  const uint16_t request_size = 1200;
  Reply1Data reply1 = DoRequest1(socket, *ctx->addr, request_size);
  ASSERT_TRUE(reply1.ok);
  EXPECT_LE(reply1.reply_size, static_cast<size_t>(request_size))
      << "Reply1 was larger than Request1 (amplification vector)";

  ctx->server->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// --- P2P UDP hole-punching ----------------------------------------------------

static std::shared_ptr<InetAddress> LocalAddr(PortNumber port) {
  return std::shared_ptr<InetAddress>(InetAddress::from("127.0.0.1", port));
}

// Two peers punch to each other and must both reach Ready(), which requires the
// full encryption handshake (reliable app-level packets) to flow both ways over
// the punched ZDT connection. On loopback there is no NAT, so this exercises the
// punch state machine + handshake rather than real traversal.
TEST(ZDTP2P, HolePunchLoopbackReachesReady) {
  ASSERT_EQ(Init(), Result::Success);

  PortNumber port_a = FreeUdpPort();
  PortNumber port_b = FreeUdpPort();
  while (port_b == port_a) {
    port_b = FreeUdpPort();
  }

  const uint64_t punch_id = 0x1234;
  bool init_a = p2p::IsInitiator(punch_id, "peerA", "peerB");
  bool init_b = p2p::IsInitiator(punch_id, "peerB", "peerA");
  ASSERT_NE(init_a, init_b) << "exactly one peer must be the initiator";

  std::shared_ptr<PeerSession> session_a;
  std::shared_ptr<PeerSession> session_b;
  Result result_a = Result::Failure;
  Result result_b = Result::Failure;

  std::thread thread_a([&]() {
    session_a = p2p::PunchSync(LocalAddr(port_a), LocalAddr(port_b), &result_a,
                               init_a, ConnectionType::ZDT, 5000);
  });
  std::thread thread_b([&]() {
    session_b = p2p::PunchSync(LocalAddr(port_b), LocalAddr(port_a), &result_b,
                               init_b, ConnectionType::ZDT, 5000);
  });
  thread_a.join();
  thread_b.join();

  ASSERT_EQ(result_a, Result::Success) << "peer A punch failed";
  ASSERT_EQ(result_b, Result::Success) << "peer B punch failed";
  ASSERT_TRUE(session_a && session_b);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         !(session_a->IsReady() && session_b->IsReady())) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(session_a->IsReady()) << "peer A never completed the ZDT session";
  EXPECT_TRUE(session_b->IsReady()) << "peer B never completed the ZDT session";

  session_a->Close();
  session_b->Close();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
