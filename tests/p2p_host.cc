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
// The shared-socket P2P host: asynchronous punches, several peers on one
// socket, and the full mesh flow through an in-process rendezvous.
//

#include "znet/init.h"
#include "znet/p2p/dialer.h"
#include "znet/p2p/host.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/rendezvous_server.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/packet_serializer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace znet;

namespace {

template <typename Pred>
bool WaitUntil(Pred pred, int ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// one StartPunch outcome, collected thread-safely off the host's thread
struct PunchOutcome {
  std::mutex mutex;
  Result result = Result::Failure;
  std::shared_ptr<PeerSession> session;
  std::atomic<bool> done{false};

  p2p::Host::PunchCallback Callback() {
    return [this](Result r, std::shared_ptr<PeerSession> s) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        result = r;
        session = std::move(s);
      }
      done = true;
    };
  }

  std::shared_ptr<PeerSession> Session() {
    std::lock_guard<std::mutex> lock(mutex);
    return session;
  }
};

std::shared_ptr<InetAddress> HostAddr(const p2p::Host& host) {
  return InetAddress::from("127.0.0.1", host.punch_port());
}

// the test packet the punched sessions speak
enum MeshPacketType : PacketId { kPacketNote = 1 };

class NotePacket : public Packet {
 public:
  NotePacket() : Packet(kPacketNote) {}
  std::string text;
};

class NoteSerializer : public PacketSerializer<NotePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<NotePacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }
  std::shared_ptr<NotePacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<NotePacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

std::shared_ptr<Codec> MakeNoteCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketNote, std::make_unique<NoteSerializer>());
  return codec;
}

class NoteCollector : public PacketHandler<NoteCollector, NotePacket> {
 public:
  void OnPacket(std::shared_ptr<NotePacket> packet) {
    std::lock_guard<std::mutex> lock(mutex);
    notes.push_back(packet->text);
    count++;
  }
  std::mutex mutex;
  std::vector<std::string> notes;
  std::atomic<int> count{0};
};

bool BothReady(PunchOutcome& a, PunchOutcome& b) {
  auto sa = a.Session();
  auto sb = b.Session();
  return sa && sb && sa->IsReady() && sb->IsReady();
}

}  // namespace

TEST(P2PHost, PunchesAsynchronouslyAndExchangesMessages) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Host::Config config;
  config.bind_address = "127.0.0.1";
  p2p::Host a{config};
  p2p::Host b{config};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);

  // a dead first candidate on each side; the race has to find the real one
  std::shared_ptr<InetAddress> dead = InetAddress::from("203.0.113.1", 9);
  PunchOutcome at_a;
  PunchOutcome at_b;
  a.StartPunch({dead, HostAddr(b)}, 7, /*is_initiator=*/true,
               std::chrono::seconds(10), at_a.Callback());
  b.StartPunch({dead, HostAddr(a)}, 7, /*is_initiator=*/false,
               std::chrono::seconds(10), at_b.Callback());

  ASSERT_TRUE(WaitUntil([&]() { return at_a.done.load() && at_b.done.load(); },
                        12000));
  ASSERT_EQ(at_a.result, Result::Success);
  ASSERT_EQ(at_b.result, Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return BothReady(at_a, at_b); }, 10000))
      << "the encrypted handshake must complete over the shared sockets";

  // both ends quiet now; install the app codec and talk
  auto collector = std::make_shared<NoteCollector>();
  at_a.Session()->SetCodec(MakeNoteCodec());
  at_b.Session()->SetCodec(MakeNoteCodec());
  at_b.Session()->SetHandler(collector);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  auto note = std::make_shared<NotePacket>();
  note->text = "hello over the punched socket";
  ASSERT_EQ(at_a.Session()->SendPacket(note), Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return collector->count.load() == 1; }, 5000));
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    EXPECT_EQ(collector->notes[0], "hello over the punched socket");
  }
  a.Stop();
  b.Stop();
}

TEST(P2PHost, ThreePeersShareOneSocketEach) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Host::Config config;
  config.bind_address = "127.0.0.1";
  p2p::Host a{config};
  p2p::Host b{config};
  p2p::Host c{config};
  ASSERT_EQ(a.Start(), Result::Success);
  ASSERT_EQ(b.Start(), Result::Success);
  ASSERT_EQ(c.Start(), Result::Success);

  // two concurrent punches from a's single socket, the thing a mesh is
  PunchOutcome a_to_b;
  PunchOutcome a_to_c;
  PunchOutcome b_to_a;
  PunchOutcome c_to_a;
  a.StartPunch({HostAddr(b)}, 1, true, std::chrono::seconds(10),
               a_to_b.Callback());
  a.StartPunch({HostAddr(c)}, 2, true, std::chrono::seconds(10),
               a_to_c.Callback());
  b.StartPunch({HostAddr(a)}, 1, false, std::chrono::seconds(10),
               b_to_a.Callback());
  c.StartPunch({HostAddr(a)}, 2, false, std::chrono::seconds(10),
               c_to_a.Callback());

  ASSERT_TRUE(WaitUntil(
      [&]() {
        return a_to_b.done.load() && a_to_c.done.load() &&
               b_to_a.done.load() && c_to_a.done.load();
      },
      12000));
  ASSERT_EQ(a_to_b.result, Result::Success);
  ASSERT_EQ(a_to_c.result, Result::Success);
  ASSERT_EQ(b_to_a.result, Result::Success);
  ASSERT_EQ(c_to_a.result, Result::Success);
  EXPECT_EQ(a.session_count(), 2u) << "both peers live on a's one socket";

  ASSERT_TRUE(WaitUntil([&]() { return BothReady(a_to_b, b_to_a); }, 10000));
  ASSERT_TRUE(WaitUntil([&]() { return BothReady(a_to_c, c_to_a); }, 10000));

  // b and c each send to a; a must keep the two streams apart
  auto collector = std::make_shared<NoteCollector>();
  a_to_b.Session()->SetCodec(MakeNoteCodec());
  a_to_b.Session()->SetHandler(collector);
  a_to_c.Session()->SetCodec(MakeNoteCodec());
  a_to_c.Session()->SetHandler(collector);
  b_to_a.Session()->SetCodec(MakeNoteCodec());
  c_to_a.Session()->SetCodec(MakeNoteCodec());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  auto from_b = std::make_shared<NotePacket>();
  from_b->text = "from b";
  auto from_c = std::make_shared<NotePacket>();
  from_c->text = "from c";
  ASSERT_EQ(b_to_a.Session()->SendPacket(from_b), Result::Success);
  ASSERT_EQ(c_to_a.Session()->SendPacket(from_c), Result::Success);
  ASSERT_TRUE(WaitUntil([&]() { return collector->count.load() == 2; }, 5000));
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    EXPECT_NE(collector->notes[0], collector->notes[1])
        << "one note from each peer, not a duplicate";
  }
  a.Stop();
  b.Stop();
  c.Stop();
}

TEST(P2PHost, PunchTowardNothingTimesOut) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::Host::Config config;
  config.bind_address = "127.0.0.1";
  p2p::Host host{config};
  ASSERT_EQ(host.Start(), Result::Success);

  PunchOutcome outcome;
  std::shared_ptr<InetAddress> dead = InetAddress::from("203.0.113.1", 9);
  host.StartPunch({dead}, 3, true, std::chrono::milliseconds(300),
                  outcome.Callback());
  ASSERT_TRUE(WaitUntil([&]() { return outcome.done.load(); }, 5000));
  EXPECT_EQ(outcome.result, Result::Timeout);
  EXPECT_FALSE(outcome.Session());
  host.Stop();
}

// --- The mesh through the rendezvous ------------------------------------------

namespace {

struct MeshProbe {
  p2p::MeshLocator locator;
  std::mutex mutex;
  std::string name;
  std::vector<std::shared_ptr<PeerSession>> sessions;
  std::atomic<int> connected{0};
  std::atomic<bool> ready{false};

  explicit MeshProbe(PortNumber relay_port)
      : locator(p2p::MeshLocator::Config{"127.0.0.1", relay_port,
                                         SessionOptions{}, "0.0.0.0", 0}) {
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
              sessions.push_back(ev.session());
            }
            connected++;
            return false;
          });
    });
  }

  std::string Name() {
    std::lock_guard<std::mutex> lock(mutex);
    return name;
  }

  bool AllSessionsReady(int expected) {
    std::lock_guard<std::mutex> lock(mutex);
    if (static_cast<int>(sessions.size()) != expected) {
      return false;
    }
    for (const auto& session : sessions) {
      if (!session->IsReady()) {
        return false;
      }
    }
    return true;
  }
};

}  // namespace

TEST(MeshLocatorEndToEnd, ThreePlayersFormAMesh) {
  ASSERT_EQ(Init(), Result::Success);
  p2p::RendezvousServer::Config config;
  config.bind_address = "127.0.0.1";
  config.bind_port = 0;
  config.punch_connection_type = ConnectionType::ZDT;
  p2p::RendezvousServer relay{config};
  ASSERT_EQ(relay.Start(), Result::Success);
  const PortNumber port = relay.bind_address()->port();

  MeshProbe a{port};
  MeshProbe b{port};
  MeshProbe c{port};
  ASSERT_EQ(a.locator.Connect(), Result::Success);
  ASSERT_EQ(b.locator.Connect(), Result::Success);
  ASSERT_EQ(c.locator.Connect(), Result::Success);
  ASSERT_TRUE(WaitUntil(
      [&]() { return a.ready.load() && b.ready.load() && c.ready.load(); },
      5000));

  // everyone asks everyone; the relay pairs the mutual asks into punches
  ASSERT_EQ(a.locator.AskPeer(b.Name()), Result::Success);
  ASSERT_EQ(a.locator.AskPeer(c.Name()), Result::Success);
  ASSERT_EQ(b.locator.AskPeer(a.Name()), Result::Success);
  ASSERT_EQ(b.locator.AskPeer(c.Name()), Result::Success);
  ASSERT_EQ(c.locator.AskPeer(a.Name()), Result::Success);
  ASSERT_EQ(c.locator.AskPeer(b.Name()), Result::Success);

  ASSERT_TRUE(WaitUntil(
      [&]() {
        return a.connected.load() == 2 && b.connected.load() == 2 &&
               c.connected.load() == 2;
      },
      20000))
      << "every pair must punch: a=" << a.connected.load()
      << " b=" << b.connected.load() << " c=" << c.connected.load();

  EXPECT_TRUE(WaitUntil([&]() { return a.AllSessionsReady(2); }, 10000));
  EXPECT_TRUE(WaitUntil([&]() { return b.AllSessionsReady(2); }, 10000));
  EXPECT_TRUE(WaitUntil([&]() { return c.AllSessionsReady(2); }, 10000));
  EXPECT_EQ(a.locator.host().session_count(), 2u);

  a.locator.Disconnect();
  b.locator.Disconnect();
  c.locator.Disconnect();
  relay.Stop();
}
