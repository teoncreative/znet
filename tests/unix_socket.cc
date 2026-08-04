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
// Unix domain socket support: address parsing, and the whole stream stack
// (framing, handshake, encryption) over an AF_UNIX pair via Server/Client
// with ConnectionType::TCP. POSIX only, like the feature.
//

#include "znet/precompiled.h"

#if ZNET_HAS_AF_UNIX

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/init.h"
#include "znet/inet_addr.h"
#include "znet/server.h"
#include "znet/server_events.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace znet;

namespace {

std::string TestSocketPath(const char* tag) {
  return "/tmp/znet-test-" + std::to_string(::getpid()) + "-" + tag + ".sock";
}

bool WaitFor(const std::atomic<bool>& flag, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return flag.load();
}

}  // namespace

TEST(UnixAddress, ParsesTheUnixScheme) {
  auto addr = InetAddress::from("unix:/tmp/some.sock", 0);
  ASSERT_TRUE(addr);
  EXPECT_TRUE(addr->is_valid());
  EXPECT_EQ(addr->ipv(), InetProtocolVersion::Unix);
  EXPECT_EQ(addr->readable(), "unix:/tmp/some.sock");
  EXPECT_EQ(addr->port(), 0);
}

TEST(UnixAddress, PortIsIgnoredAndPreservedAcrossWithPort) {
  auto addr = InetAddress::from("unix:/tmp/some.sock", 4242);
  ASSERT_TRUE(addr);
  auto same = addr->WithPort(9);
  ASSERT_TRUE(same);
  EXPECT_EQ(same->readable(), "unix:/tmp/some.sock");
  EXPECT_EQ(same->port(), 0);
}

TEST(UnixAddress, RefusesAPathLongerThanSunPath) {
  const std::string long_path(sizeof(sockaddr_un{}.sun_path), 'x');
  auto addr = InetAddress::from("unix:" + long_path, 0);
  ASSERT_TRUE(addr);
  EXPECT_FALSE(addr->is_valid());
}

TEST(UnixAddress, RoundTripsThroughSockaddr) {
  auto addr = InetAddress::from("unix:/tmp/some.sock", 0);
  ASSERT_TRUE(addr);
  auto back = InetAddress::from(const_cast<sockaddr*>(addr->handle_ptr()));
  ASSERT_TRUE(back);
  EXPECT_EQ(back->readable(), "unix:/tmp/some.sock");
}

TEST(UnixSocket, ZDTRefusesAUnixAddress) {
  ASSERT_EQ(Init(), Result::Success);
  ServerConfig config{TestSocketPath("zdt").insert(0, "unix:"), 0,
                      std::chrono::seconds(2), ConnectionType::ZDT};
  Server server{config};
  EXPECT_NE(server.Bind(), Result::Success);
}

TEST(UnixSocket, ServerAndClientTalkOverAPath) {
  ASSERT_EQ(Init(), Result::Success);
  const std::string path = TestSocketPath("talk");

  ServerConfig server_config{"unix:" + path, 0, std::chrono::seconds(5),
                             ConnectionType::TCP};
  Server server{server_config};
  std::atomic<bool> server_saw_client{false};
  server.SetEventCallback([&](Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.Dispatch<IncomingClientConnectedEvent>(
        [&](IncomingClientConnectedEvent&) {
          server_saw_client = true;
          return false;
        });
  });
  ASSERT_EQ(server.Bind(), Result::Success);
  ASSERT_EQ(server.Listen(), Result::Success);

  ClientConfig client_config{"unix:" + path, 0, std::chrono::seconds(5),
                             ConnectionType::TCP};
  Client client{client_config};
  std::atomic<bool> client_connected{false};
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

  EXPECT_TRUE(WaitFor(client_connected, 5000))
      << "the encrypted handshake must complete over AF_UNIX";
  EXPECT_TRUE(WaitFor(server_saw_client, 5000));

  client.Disconnect();
  server.Stop();
  client.Wait();
  server.Wait();
}

TEST(UnixSocket, StaleSocketFileDoesNotBlockRebinding) {
  ASSERT_EQ(Init(), Result::Success);
  const std::string path = TestSocketPath("stale");
  // a dead listener's socket file, as left by a crash
  SocketHandle stale = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_TRUE(IsValidSocketHandle(stale));
  auto addr = InetAddress::from("unix:" + path, 0);
  ASSERT_TRUE(addr);
  ASSERT_EQ(bind(stale, addr->handle_ptr(), addr->addr_size()), 0);
  CloseSocket(stale);  // closes the socket, leaves the file

  ServerConfig config{"unix:" + path, 0, std::chrono::seconds(2),
                      ConnectionType::TCP};
  Server server{config};
  EXPECT_EQ(server.Bind(), Result::Success)
      << "Bind() must take over a stale socket file";
  server.Stop();
}

#endif  // ZNET_HAS_AF_UNIX
