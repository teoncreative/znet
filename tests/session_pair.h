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
// Two real PeerSessions joined by a hand-pumped fake wire. Grew up in the
// encryption tests and is shared by anything that needs a session pair without
// sockets: the test moves frames between the ends itself, in whatever order
// and shape it wants to model.
//

#pragma once

#include "znet/codec.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/transport.h"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <vector>

// test-only header, so the shorthand every test file already uses is fine here
using namespace znet;


// A transport that goes nowhere: Send() parks the encoded frame, Receive()
// hands back whatever the test put in the inbox. The test moves frames between
// the two ends itself, in whatever order it wants to model.
class FakeTransport : public TransportLayer {
 public:
  struct Frame {
    std::shared_ptr<Buffer> buffer;
    uint8_t channel;
  };

  std::shared_ptr<Buffer> Receive() override {
    if (inbox.empty()) {
      return nullptr;
    }
    auto buffer = inbox.front();
    inbox.pop_front();
    return buffer;
  }
  bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) override {
    sent.push_back(Frame{std::move(buffer), OrderingDomain(options)});
    return true;
  }
  // models a transport that orders each channel on its own, the way ZDT does;
  // the single-stream default would make the reordering tests vacuous
  uint8_t OrderingDomain(const SendOptions& options) const override {
    return options.GetOr<ChannelKey>(0);
  }
  Result Close(CloseOptions = {}) override {
    closed = true;
    return Result::Success;
  }
  bool IsClosed() override { return closed; }
  void Update() override {}
  void Flush() override {}

  std::vector<Frame> sent;
  std::deque<std::shared_ptr<Buffer>> inbox;
  bool closed = false;
};

enum TestPacketType : PacketId { kPacketProbe = 1 };

class ProbePacket : public Packet {
 public:
  ProbePacket() : Packet(kPacketProbe) {}
  uint32_t seq = 0;
};

class ProbeSerializer : public PacketSerializer<ProbePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ProbePacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->seq);
    return buffer;
  }
  std::shared_ptr<ProbePacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ProbePacket>();
    packet->seq = buffer->ReadInt<uint32_t>();
    return packet;
  }
};

std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketProbe, std::make_unique<ProbeSerializer>());
  return codec;
}

class CollectHandler : public PacketHandler<CollectHandler, ProbePacket> {
 public:
  explicit CollectHandler(std::vector<uint32_t>* got) : got_(got) {}
  void OnPacket(std::shared_ptr<ProbePacket> packet) {
    got_->push_back(packet->seq);
  }

 private:
  std::vector<uint32_t>* got_;
};

// Two sessions and the wire between them. `encryption` picks what the accepting
// side announces; the initiator adopts it, as it would over a socket.
struct Pair {
  FakeTransport* client_wire = nullptr;
  FakeTransport* server_wire = nullptr;
  std::unique_ptr<PeerSession> client;
  std::unique_ptr<PeerSession> server;
  std::vector<uint32_t> server_got;

  // `base` carries any further per-session options a test wants to exercise;
  // encryption and compression are pinned here because most tests assume them
  explicit Pair(bool encryption = true,
                const SessionOptions& base = SessionOptions()) {
    auto client_transport = std::unique_ptr<FakeTransport>(new FakeTransport());
    auto server_transport = std::unique_ptr<FakeTransport>(new FakeTransport());
    client_wire = client_transport.get();
    server_wire = server_transport.get();

    SessionOptions options = base;
    options.common.encryption = encryption;
    options.common.compression = CompressionType::None;

    std::shared_ptr<InetAddress> client_addr =
        InetAddress::from("127.0.0.1", 1000);
    std::shared_ptr<InetAddress> server_addr =
        InetAddress::from("127.0.0.1", 2000);
    client.reset(new PeerSession(client_addr, server_addr,
                                 std::move(client_transport),
                                 ConnectionType::ZDT, /*is_initiator=*/true,
                                 /*self_managed=*/false, options));
    server.reset(new PeerSession(server_addr, client_addr,
                                 std::move(server_transport),
                                 ConnectionType::ZDT, /*is_initiator=*/false,
                                 /*self_managed=*/false, options));
  }

  // Moves every parked frame across and lets both ends process it.
  void Pump() {
    for (auto& frame : client_wire->sent) {
      server_wire->inbox.push_back(frame.buffer);
    }
    client_wire->sent.clear();
    for (auto& frame : server_wire->sent) {
      client_wire->inbox.push_back(frame.buffer);
    }
    server_wire->sent.clear();
    server->Process();
    client->Process();
  }

  bool Handshake() {
    for (int i = 0; i < 20 && !(client->IsReady() && server->IsReady()); i++) {
      Pump();
    }
    if (!client->IsReady() || !server->IsReady()) {
      return false;
    }
    // the handshake replaces both, so the application's codec goes on after it
    client->SetCodec(MakeCodec());
    server->SetCodec(MakeCodec());
    server->SetHandler(std::make_shared<CollectHandler>(&server_got));
    client->SetHandler(std::make_shared<CollectHandler>(&server_got));
    client_wire->sent.clear();
    server_wire->sent.clear();
    return true;
  }

  // Encodes one packet and returns the frame, without delivering it.
  FakeTransport::Frame Emit(uint32_t seq, uint8_t channel) {
    auto packet = std::make_shared<ProbePacket>();
    packet->seq = seq;
    SendOptions options;
    options.Set<ChannelKey>(channel);
    EXPECT_TRUE(client->SendPacket(packet, options));
    client->DrainOutbound();
    EXPECT_FALSE(client_wire->sent.empty());
    FakeTransport::Frame frame = client_wire->sent.back();
    client_wire->sent.clear();
    return frame;
  }

  void Deliver(const FakeTransport::Frame& frame) {
    server_wire->inbox.push_back(frame.buffer);
    server->Process();
  }

  // Delivering consumes a buffer's read cursor, so a frame that is meant to be
  // sent twice has to be snapshotted first: reusing the object would replay a
  // spent buffer rather than the bytes an attacker actually captured.
  static FakeTransport::Frame Snapshot(const FakeTransport::Frame& frame) {
    return FakeTransport::Frame{
        // the readable region, not data(): an in-place-framed buffer carries
        // unspent headroom in front of the actual frame
        std::make_shared<Buffer>(frame.buffer->read_cursor_data(),
                                 frame.buffer->readable_bytes()),
        frame.channel};
  }
};
