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
// PeerLocator tests: lifecycle (the destructor has to wake the worker out of
// its condition wait and join it; a regression here is a hang, so lifecycle
// cases run on a helper thread and fail on a deadline instead of deadlocking
// the suite), and the whole rendezvous-and-punch flow end to end against an
// in-process RendezvousServer on loopback.
//

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/init.h"
#include "znet/p2p/dialer.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/rendezvous_server.h"
#include "znet/packet_handler.h"

#include <vector>

using namespace znet;

namespace {

// Runs fn on its own thread; returns false if it did not finish in time.
// The thread is leaked on timeout, which is fine for a failing test.
bool RunWithDeadline(std::function<void()> fn, std::chrono::seconds deadline) {
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> fut = done->get_future();
  std::thread worker([done, fn]() {
    fn();
    done->set_value();
  });
  if (fut.wait_for(deadline) == std::future_status::ready) {
    worker.join();
    return true;
  }
  worker.detach();
  return false;
}

}  // namespace

TEST(PeerLocator, DestructorJoinsWorkerWithoutConnect) {
  ASSERT_EQ(znet::Init(), znet::Result::Success);
  bool finished = RunWithDeadline(
      []() {
        znet::p2p::PeerLocatorConfig config;
        config.server_ip = "127.0.0.1";
        config.server_port = 1;
        znet::p2p::PeerLocator locator{config};
      },
      std::chrono::seconds(10));
  EXPECT_TRUE(finished) << "destructor hung with no worker started";
}

TEST(PeerLocator, DestructorJoinsWorkerAfterFailedConnect) {
  ASSERT_EQ(znet::Init(), znet::Result::Success);
  bool finished = RunWithDeadline(
      []() {
        znet::p2p::PeerLocatorConfig config;
        // Nothing listens on port 1; loopback refuses immediately.
        config.server_ip = "127.0.0.1";
        config.server_port = 1;
        znet::p2p::PeerLocator locator{config};
        znet::Result result = locator.Connect();
        EXPECT_NE(result, znet::Result::Success);
      },
      std::chrono::seconds(10));
  EXPECT_TRUE(finished) << "destructor hung joining the worker thread";
}

TEST(PeerLocator, ConnectCanBeTriedAgainAfterAFailure) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::PeerLocatorConfig config;
  config.server_ip = "127.0.0.1";
  config.server_port = 1;
  p2p::PeerLocator locator{config};
  EXPECT_NE(locator.Connect(), Result::Success);
  // a failed attempt used to leave is_running_ set and a worker parked, which
  // turned every later Connect() into AlreadyConnected
  EXPECT_NE(locator.Connect(), Result::AlreadyConnected);
}

// --- End to end over an in-process rendezvous ---------------------------------

namespace {

bool WaitFor(const std::atomic<bool>& flag, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return flag.load();
}

// one locator plus everything its events reported, collected thread-safely:
// ready/connected/failed fire on the relay-client thread or the worker
struct LocatorProbe {
  p2p::PeerLocator locator;
  std::mutex mutex;
  std::string name;
  std::shared_ptr<PeerSession> session;
  p2p::PeerLocatorPhase failed_phase{};
  Result failed_reason{};
  std::string failed_target;
  std::atomic<bool> ready{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> closed{false};

  explicit LocatorProbe(PortNumber relay_port)
      : locator(p2p::PeerLocatorConfig{"127.0.0.1", relay_port}) {
    locator.SetEventCallback([this](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<p2p::PeerLocatorReadyEvent>(
          [this](p2p::PeerLocatorReadyEvent& ev) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              name = ev.peer_name();
            }
            ready = true;
            return false;
          });
      dispatcher.Dispatch<p2p::PeerConnectedEvent>(
          [this](p2p::PeerConnectedEvent& ev) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              session = ev.session();
            }
            connected = true;
            return false;
          });
      dispatcher.Dispatch<p2p::PeerLocatorFailedEvent>(
          [this](p2p::PeerLocatorFailedEvent& ev) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              failed_phase = ev.phase();
              failed_reason = ev.reason();
              failed_target = ev.target_peer();
            }
            failed = true;
            return false;
          });
      dispatcher.Dispatch<p2p::PeerLocatorCloseEvent>(
          [this](p2p::PeerLocatorCloseEvent&) {
            closed = true;
            return false;
          });
    });
  }

  std::string Name() {
    std::lock_guard<std::mutex> lock(mutex);
    return name;
  }

  std::shared_ptr<PeerSession> Session() {
    std::lock_guard<std::mutex> lock(mutex);
    return session;
  }
};

void RunPunchEndToEnd(ConnectionType punch_type) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_ip = "127.0.0.1";
  config.bind_port = 0;
  config.punch_connection_type = punch_type;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);
  const PortNumber port = relay.bind_address()->port();
  ASSERT_NE(port, 0);

  LocatorProbe a{port};
  LocatorProbe b{port};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_EQ(b.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.ready, 5000)) << "a never got a peer name";
  ASSERT_TRUE(WaitFor(b.ready, 5000)) << "b never got a peer name";

  ASSERT_EQ(a.locator.AskPeer(b.Name()), Result::Success);
  ASSERT_EQ(b.locator.AskPeer(a.Name()), Result::Success);

  ASSERT_TRUE(WaitFor(a.connected, 15000)) << "a's punch never completed";
  ASSERT_TRUE(WaitFor(b.connected, 15000)) << "b's punch never completed";

  // the punched sessions drive themselves; the post-punch handshake has to
  // settle on both ends
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while ((!a.Session()->IsReady() || !b.Session()->IsReady()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(a.Session()->IsReady());
  EXPECT_TRUE(b.Session()->IsReady());
  relay.Stop();
}

}  // namespace

TEST(PeerLocatorEndToEnd, PunchesOverTCP) {
  RunPunchEndToEnd(ConnectionType::TCP);
}

#ifndef TARGET_WIN
// std::clock() is process CPU time on POSIX, which is exactly the claim under
// test; on Windows it is wall time and the test would be meaningless.
TEST(PeerLocatorEndToEnd, IdlePunchedSessionsDoNotSpin) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_ip = "127.0.0.1";
  config.bind_port = 0;
  config.punch_connection_type = ConnectionType::ZDT;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);
  const PortNumber port = relay.bind_address()->port();

  LocatorProbe a{port};
  LocatorProbe b{port};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_EQ(b.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.ready, 5000));
  ASSERT_TRUE(WaitFor(b.ready, 5000));
  ASSERT_EQ(a.locator.AskPeer(b.Name()), Result::Success);
  ASSERT_EQ(b.locator.AskPeer(a.Name()), Result::Success);
  ASSERT_TRUE(WaitFor(a.connected, 15000));
  ASSERT_TRUE(WaitFor(b.connected, 15000));

  // two idle self-managed sessions for one wall second. Spinning loops would
  // cost ~two CPU seconds; paced ones cost almost nothing.
  const std::clock_t cpu_before = std::clock();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const double cpu_seconds =
      static_cast<double>(std::clock() - cpu_before) / CLOCKS_PER_SEC;
  EXPECT_LT(cpu_seconds, 0.5)
      << "idle punched sessions must doze, not spin a core";
  relay.Stop();
}
#endif  // TARGET_WIN

TEST(PeerLocatorEndToEnd, PunchesOverZDT) {
  RunPunchEndToEnd(ConnectionType::ZDT);
}

// --- Relay protections --------------------------------------------------------

namespace {

// speaks the rendezvous protocol by hand, so it can misbehave in ways
// PeerLocator never would
struct RawRelayClient {
  Client client;
  std::mutex mutex;
  std::vector<std::string> names;
  std::shared_ptr<PeerSession> session;
  std::atomic<bool> connected{false};
  std::atomic<int> name_count{0};

  explicit RawRelayClient(PortNumber port, int timeout_seconds = 5)
      : client(ClientConfig{"127.0.0.1", port,
                            std::chrono::seconds(timeout_seconds),
                            ConnectionType::TCP, {}}) {
    client.SetEventCallback([this](Event& event) {
      EventDispatcher dispatcher{event};
      dispatcher.Dispatch<ClientConnectedToServerEvent>(
          [this](ClientConnectedToServerEvent& ev) {
            auto handler = std::make_shared<CallbackPacketHandler>();
            handler->AddRef<p2p::SetPeerNamePacket>(
                [this](const p2p::SetPeerNamePacket& pk) {
                  {
                    std::lock_guard<std::mutex> lock(mutex);
                    names.push_back(pk.peer_name_);
                  }
                  name_count++;
                });
            auto s = ev.session();
            s->SetCodec(p2p::BuildCodec());
            s->SetHandler(handler);
            {
              std::lock_guard<std::mutex> lock(mutex);
              session = s;
            }
            connected = true;
            return false;
          });
    });
  }

  std::shared_ptr<PeerSession> Session() {
    std::lock_guard<std::mutex> lock(mutex);
    return session;
  }
};

template <typename Pred>
bool WaitUntil(Pred pred, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

}  // namespace

TEST(RendezvousProtection, RepeatedIdentifyKeepsTheSameName) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_ip = "127.0.0.1";
  config.bind_port = 0;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);

  RawRelayClient c{relay.bind_address()->port()};
  ASSERT_EQ(c.client.Bind(), Result::Success);
  ASSERT_EQ(c.client.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(c.connected, 5000));

  c.Session()->SendPacket(std::make_shared<p2p::IdentifyPacket>());
  ASSERT_TRUE(WaitUntil([&]() { return c.name_count.load() >= 1; }, 5000));
  c.Session()->SendPacket(std::make_shared<p2p::IdentifyPacket>());
  ASSERT_TRUE(WaitUntil([&]() { return c.name_count.load() >= 2; }, 5000));
  {
    std::lock_guard<std::mutex> lock(c.mutex);
    EXPECT_EQ(c.names[0], c.names[1])
        << "a repeat identify must not mint (and leak) a fresh name";
  }
  c.client.Disconnect();
  relay.Stop();
}

TEST(RendezvousProtection, RequestSpamGetsTheClientDisconnected) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_ip = "127.0.0.1";
  config.bind_port = 0;
  config.max_requests_per_window = 3;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);

  RawRelayClient c{relay.bind_address()->port()};
  ASSERT_EQ(c.client.Bind(), Result::Success);
  ASSERT_EQ(c.client.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(c.connected, 5000));

  for (int i = 0; i < 10; i++) {
    auto ask = std::make_shared<p2p::ConnectPeerPacket>();
    ask->target_peer_ = "whoever";
    c.Session()->SendPacket(ask);
  }
  EXPECT_TRUE(WaitUntil([&]() { return !c.Session()->IsAlive(); }, 5000))
      << "the relay must drop a client that spams it";
  c.client.Disconnect();
  relay.Stop();
}

TEST(RendezvousProtection, RelayHonorsServerOptions) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_ip = "127.0.0.1";
  config.bind_port = 0;
  config.server_options.denylist.push_back(CIDRBlock::Parse("127.0.0.0/8"));
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);

  RawRelayClient c{relay.bind_address()->port(), /*timeout_seconds=*/1};
  ASSERT_EQ(c.client.Bind(), Result::Success);
  (void)c.client.Connect();
  EXPECT_FALSE(WaitFor(c.connected, 1500))
      << "the relay listener must honor its admission rules";
  relay.Stop();
}

// --- Candidate racing ---------------------------------------------------------

namespace {

PortNumber FreePortLocal(int socket_type) {
  SocketHandle probe = socket(AF_INET, socket_type, 0);
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

// both peers punch with a dead first candidate; only the second one is the
// other peer. The punch must find it inside the budget.
void RunCandidateRace(ConnectionType type, PortNumber port_a,
                      PortNumber port_b) {
  std::shared_ptr<InetAddress> local_a = InetAddress::from("127.0.0.1", port_a);
  std::shared_ptr<InetAddress> local_b = InetAddress::from("127.0.0.1", port_b);
  // TEST-NET-1 space: routable nowhere, so the candidate just goes dark
  std::shared_ptr<InetAddress> dead = InetAddress::from("203.0.113.1", 9);
  std::vector<std::shared_ptr<InetAddress>> to_b = {dead, local_b};
  std::vector<std::shared_ptr<InetAddress>> to_a = {dead, local_a};

  Result result_a = Result::Failure;
  Result result_b = Result::Failure;
  std::shared_ptr<PeerSession> session_a;
  std::shared_ptr<PeerSession> session_b;
  std::thread thread_a([&]() {
    session_a = p2p::PunchSync(local_a, to_b, &result_a,
                               /*is_initiator=*/true, type, 10000);
  });
  std::thread thread_b([&]() {
    session_b = p2p::PunchSync(local_b, to_a, &result_b,
                               /*is_initiator=*/false, type, 10000);
  });
  thread_a.join();
  thread_b.join();
  EXPECT_EQ(result_a, Result::Success) << "initiator punch failed";
  EXPECT_EQ(result_b, Result::Success) << "responder punch failed";
  ASSERT_TRUE(session_a);
  ASSERT_TRUE(session_b);
  EXPECT_TRUE(session_a->IsAlive());
  EXPECT_TRUE(session_b->IsAlive());
}

}  // namespace

TEST(PunchCandidates, ZDTRacesPastADeadCandidate) {
  ASSERT_EQ(Init(), Result::Success);
  RunCandidateRace(ConnectionType::ZDT, FreePortLocal(SOCK_DGRAM),
                   FreePortLocal(SOCK_DGRAM));
}

TEST(PunchCandidates, TCPCyclesPastADeadCandidate) {
  ASSERT_EQ(Init(), Result::Success);
  RunCandidateRace(ConnectionType::TCP, FreePortLocal(SOCK_STREAM),
                   FreePortLocal(SOCK_STREAM));
}

TEST(PeerLocatorEndToEnd, AskingForAnUnknownPeerFailsTheRendezvous) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_ip = "127.0.0.1";
  config.bind_port = 0;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);
  const PortNumber port = relay.bind_address()->port();

  LocatorProbe a{port};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitFor(a.ready, 5000));

  ASSERT_EQ(a.locator.AskPeer("no-such-peer"), Result::Success);
  ASSERT_TRUE(WaitFor(a.failed, 5000))
      << "the relay must answer an unknown name, not stay silent";
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    EXPECT_EQ(a.failed_phase, p2p::PeerLocatorPhase::Rendezvous);
    EXPECT_EQ(a.failed_reason, Result::PeerNotFound);
    EXPECT_EQ(a.failed_target, "no-such-peer");
  }
  EXPECT_FALSE(a.connected.load());
  // the relay link survives the miss, so asking again stays possible
  a.locator.Disconnect();
  relay.Stop();
}
