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
// Admission control: CIDR parsing and matching, the AdmissionControl verdicts,
// and the rules applied end-to-end at a TCP accept and a ZDT handshake.
//

#include "znet/admission.h"
#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/init.h"
#include "znet/inet_addr.h"
#include "znet/server.h"
#include "znet/server_events.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace znet;
using backends::AdmissionControl;

namespace {

std::unique_ptr<InetAddress> Addr(const char* ip) {
  return InetAddress::from(ip, 12345);
}

bool WaitFor(const std::atomic<bool>& flag, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return flag.load();
}

PortNumber FreeTcpPort() {
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

TEST(CIDRBlockTest, ParsesTheUsualShapes) {
  EXPECT_TRUE(CIDRBlock::Parse("10.0.0.0/8").is_valid());
  EXPECT_TRUE(CIDRBlock::Parse("192.168.1.5").is_valid());
  EXPECT_TRUE(CIDRBlock::Parse("0.0.0.0/0").is_valid());
  EXPECT_TRUE(CIDRBlock::Parse("2001:db8::/32").is_valid());
  EXPECT_TRUE(CIDRBlock::Parse("::1").is_valid());
}

TEST(CIDRBlockTest, RefusesTheBrokenShapes) {
  EXPECT_FALSE(CIDRBlock::Parse("").is_valid());
  EXPECT_FALSE(CIDRBlock::Parse("not an ip").is_valid());
  EXPECT_FALSE(CIDRBlock::Parse("10.0.0.0/33").is_valid());
  EXPECT_FALSE(CIDRBlock::Parse("::1/129").is_valid());
  EXPECT_FALSE(CIDRBlock::Parse("10.0.0.0/").is_valid());
  EXPECT_FALSE(CIDRBlock::Parse("10.0.0.0/8x").is_valid());
  EXPECT_FALSE(CIDRBlock::Parse("10.0.0.0/-1").is_valid());
}

TEST(CIDRBlockTest, MatchesInsideThePrefixOnly) {
  const CIDRBlock ten_eight = CIDRBlock::Parse("10.0.0.0/8");
  EXPECT_TRUE(ten_eight.Matches(*Addr("10.0.0.1")));
  EXPECT_TRUE(ten_eight.Matches(*Addr("10.255.255.255")));
  EXPECT_FALSE(ten_eight.Matches(*Addr("11.0.0.1")));

  const CIDRBlock host = CIDRBlock::Parse("192.168.1.5");
  EXPECT_TRUE(host.Matches(*Addr("192.168.1.5")));
  EXPECT_FALSE(host.Matches(*Addr("192.168.1.6")));

  // /31 splits inside a byte, which is what exercises the remainder mask
  const CIDRBlock pair = CIDRBlock::Parse("192.168.1.4/31");
  EXPECT_TRUE(pair.Matches(*Addr("192.168.1.4")));
  EXPECT_TRUE(pair.Matches(*Addr("192.168.1.5")));
  EXPECT_FALSE(pair.Matches(*Addr("192.168.1.6")));
}

TEST(CIDRBlockTest, FamiliesStayApartExceptForMappedV4) {
  const CIDRBlock v4 = CIDRBlock::Parse("10.0.0.0/8");
  const CIDRBlock v6 = CIDRBlock::Parse("2001:db8::/32");
  EXPECT_FALSE(v4.Matches(*Addr("2001:db8::1")));
  EXPECT_FALSE(v6.Matches(*Addr("10.0.0.1")));
  EXPECT_TRUE(v6.Matches(*Addr("2001:db8:1::1")));

  // a v4 client on a dual-stack listener arrives as ::ffff:a.b.c.d and must
  // still be caught by the v4 rule that names it
  EXPECT_TRUE(v4.Matches(*Addr("::ffff:10.1.2.3")));
  EXPECT_FALSE(v4.Matches(*Addr("::ffff:11.1.2.3")));
}

// host_key() is what both the CIDR matcher and the throttle identify a host
// by: address bytes only, no port, one identity across the two families.
TEST(HostKeyTest, NamesTheHostNotTheConnection) {
  EXPECT_EQ(InetAddress::from("1.2.3.4", 100)->host_key(),
            InetAddress::from("1.2.3.4", 200)->host_key())
      << "the port must not change the key";
  EXPECT_EQ(InetAddress::from("::ffff:1.2.3.4", 100)->host_key(),
            InetAddress::from("1.2.3.4", 200)->host_key())
      << "a mapped v4 is the v4 it carries";
  EXPECT_NE(InetAddress::from("1.2.3.4", 100)->host_key(),
            InetAddress::from("1.2.3.5", 100)->host_key());
  EXPECT_EQ(InetAddress::from("1.2.3.4", 1)->host_key().size(), 4u);
  EXPECT_EQ(InetAddress::from("2001:db8::1", 1)->host_key().size(), 16u);
}

TEST(AdmissionControlTest, DenylistWinsOverAllowlist) {
  ServerOptions options;
  options.allowlist.push_back(CIDRBlock::Parse("10.0.0.0/8"));
  options.denylist.push_back(CIDRBlock::Parse("10.5.0.0/16"));
  AdmissionControl admission(options);

  EXPECT_EQ(admission.Admit(*Addr("10.1.0.1")), AdmissionControl::Verdict::Allow);
  EXPECT_EQ(admission.Admit(*Addr("10.5.0.1")),
            AdmissionControl::Verdict::Denylisted);
  EXPECT_EQ(admission.Admit(*Addr("192.168.0.1")),
            AdmissionControl::Verdict::NotAllowlisted);
}

TEST(AdmissionControlTest, EmptyListsAdmitEveryone) {
  AdmissionControl admission{ServerOptions{}};
  EXPECT_EQ(admission.Admit(*Addr("203.0.113.7")),
            AdmissionControl::Verdict::Allow);
}

TEST(AdmissionControlTest, ThrottleCountsPerSourceInsideTheWindow) {
  ServerOptions options;
  options.max_attempts_per_source = 2;
  options.attempt_window = std::chrono::milliseconds(100);
  AdmissionControl admission(options);

  EXPECT_EQ(admission.Admit(*Addr("10.0.0.1")), AdmissionControl::Verdict::Allow);
  EXPECT_EQ(admission.Admit(*Addr("10.0.0.1")), AdmissionControl::Verdict::Allow);
  EXPECT_EQ(admission.Admit(*Addr("10.0.0.1")),
            AdmissionControl::Verdict::Throttled);
  // another source has its own window
  EXPECT_EQ(admission.Admit(*Addr("10.0.0.2")), AdmissionControl::Verdict::Allow);

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(admission.Admit(*Addr("10.0.0.1")), AdmissionControl::Verdict::Allow)
      << "an elapsed window starts over";
}

TEST(AdmissionControlTest, InvalidBlocksAreDroppedNotEnforced) {
  ServerOptions options;
  options.denylist.push_back(CIDRBlock::Parse("garbage"));
  AdmissionControl admission(options);
  EXPECT_EQ(admission.Admit(*Addr("10.0.0.1")), AdmissionControl::Verdict::Allow);
}

// --- End to end ---------------------------------------------------------------

namespace {

struct TestServer {
  Server server;
  std::atomic<bool> saw_client{false};

  explicit TestServer(const ServerConfig& config) : server(config) {
    server.SetEventCallback([this](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<IncomingClientConnectedEvent>(
          [this](IncomingClientConnectedEvent&) {
            saw_client = true;
            return false;
          });
    });
  }
};

struct TestClient {
  Client client;
  std::atomic<bool> connected{false};

  explicit TestClient(const ClientConfig& config) : client(config) {
    client.SetEventCallback([this](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<ClientConnectedToServerEvent>(
          [this](ClientConnectedToServerEvent&) {
            connected = true;
            return false;
          });
    });
  }
};

}  // namespace

TEST(AdmissionEndToEnd, TCPDenylistRefusesTheHandshake) {
  ASSERT_EQ(Init(), Result::Success);
  const PortNumber port = FreeTcpPort();
  ASSERT_NE(port, 0);

  ServerConfig config{"127.0.0.1", port, std::chrono::seconds(2),
                      ConnectionType::TCP};
  config.options.denylist.push_back(CIDRBlock::Parse("127.0.0.0/8"));
  TestServer server{config};
  ASSERT_EQ(server.server.Bind(), Result::Success);
  ASSERT_EQ(server.server.Listen(), Result::Success);

  TestClient client{ClientConfig{"127.0.0.1", port, std::chrono::seconds(2),
                                 ConnectionType::TCP}};
  ASSERT_EQ(client.client.Bind(), Result::Success);
  // the TCP connect itself lands in the accept queue, so it succeeds; the
  // refusal closes the socket before a session or handshake exists
  (void)client.client.Connect();

  EXPECT_FALSE(WaitFor(client.connected, 1000))
      << "a denylisted source must never complete the handshake";
  EXPECT_FALSE(server.saw_client.load());

  client.client.Disconnect();
  server.server.Stop();
  client.client.Wait();
  server.server.Wait();
}

TEST(AdmissionEndToEnd, TCPAllowlistAdmitsTheNamedSource) {
  ASSERT_EQ(Init(), Result::Success);
  const PortNumber port = FreeTcpPort();
  ASSERT_NE(port, 0);

  ServerConfig config{"127.0.0.1", port, std::chrono::seconds(5),
                      ConnectionType::TCP};
  config.options.allowlist.push_back(CIDRBlock::Parse("127.0.0.0/8"));
  TestServer server{config};
  ASSERT_EQ(server.server.Bind(), Result::Success);
  ASSERT_EQ(server.server.Listen(), Result::Success);

  TestClient client{ClientConfig{"127.0.0.1", port, std::chrono::seconds(5),
                                 ConnectionType::TCP}};
  ASSERT_EQ(client.client.Bind(), Result::Success);
  ASSERT_EQ(client.client.Connect(), Result::Success);

  EXPECT_TRUE(WaitFor(client.connected, 5000));
  EXPECT_TRUE(WaitFor(server.saw_client, 5000));

  client.client.Disconnect();
  server.server.Stop();
  client.client.Wait();
  server.server.Wait();
}

TEST(AdmissionEndToEnd, TCPThrottleRefusesTheSecondAttempt) {
  ASSERT_EQ(Init(), Result::Success);
  const PortNumber port = FreeTcpPort();
  ASSERT_NE(port, 0);

  ServerConfig config{"127.0.0.1", port, std::chrono::seconds(5),
                      ConnectionType::TCP};
  config.options.max_attempts_per_source = 1;
  config.options.attempt_window = std::chrono::seconds(30);
  TestServer server{config};
  ASSERT_EQ(server.server.Bind(), Result::Success);
  ASSERT_EQ(server.server.Listen(), Result::Success);

  TestClient first{ClientConfig{"127.0.0.1", port, std::chrono::seconds(5),
                                ConnectionType::TCP}};
  ASSERT_EQ(first.client.Bind(), Result::Success);
  ASSERT_EQ(first.client.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(first.connected, 5000));

  TestClient second{ClientConfig{"127.0.0.1", port, std::chrono::seconds(2),
                                 ConnectionType::TCP}};
  ASSERT_EQ(second.client.Bind(), Result::Success);
  (void)second.client.Connect();
  EXPECT_FALSE(WaitFor(second.connected, 1000))
      << "the same source's second attempt inside the window is refused";

  first.client.Disconnect();
  second.client.Disconnect();
  server.server.Stop();
  first.client.Wait();
  second.client.Wait();
  server.server.Wait();
}

TEST(AdmissionEndToEnd, ZDTDenylistDropsTheHandshakeSilently) {
  ASSERT_EQ(Init(), Result::Success);

  ServerConfig config{"127.0.0.1", 0, std::chrono::seconds(2),
                      ConnectionType::ZDT};
  config.options.denylist.push_back(CIDRBlock::Parse("127.0.0.0/8"));
  TestServer server{config};
  ASSERT_EQ(server.server.Bind(), Result::Success);
  // port 0 resolved at bind; read it back for the client
  const PortNumber port = server.server.bind_address()->port();
  ASSERT_NE(port, 0);
  ASSERT_EQ(server.server.Listen(), Result::Success);

  TestClient client{ClientConfig{"127.0.0.1", port, std::chrono::seconds(2),
                                 ConnectionType::ZDT}};
  ASSERT_EQ(client.client.Bind(), Result::Success);
  EXPECT_NE(client.client.Connect(), Result::Success)
      << "every handshake datagram is dropped, so the connect must time out";
  EXPECT_FALSE(server.saw_client.load());

  server.server.Stop();
  server.server.Wait();
}
