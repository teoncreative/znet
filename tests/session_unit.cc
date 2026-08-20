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
// Socket-free tests for the session's send-path plumbing: the lock-free queue
// underneath it, the arbitration built on top, and the codec's framing in both
// directions, including what the decode side counts against a misbehaving
// peer. All are reachable only through a live connection otherwise, which is
// why the race that made throughput bimodal showed up as a benchmark artifact
// rather than a failure, and why a serializer breaking the frame header went
// unnoticed.
//

#include "session_pair.h"

#include "znet/codec.h"
#include "znet/init.h"
#include "znet/mpsc_queue.h"
#include "znet/outbound_queue.h"
#include "znet/packet_serializer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace znet;

// --- MpscQueue ----------------------------------------------------------------

TEST(MpscQueueTest, RoundsCapacityUpToAPowerOfTwo) {
  EXPECT_EQ(MpscQueue<int>(1).capacity(), 2u) << "minimum is two";
  EXPECT_EQ(MpscQueue<int>(2).capacity(), 2u);
  EXPECT_EQ(MpscQueue<int>(3).capacity(), 4u);
  EXPECT_EQ(MpscQueue<int>(100).capacity(), 128u)
      << "rounded up, never down: the figure is a limit callers were promised";
}

TEST(MpscQueueTest, PreservesPushOrder) {
  MpscQueue<int> q(16);
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(q.Push(i));
  }
  for (int i = 0; i < 10; i++) {
    int out = -1;
    ASSERT_TRUE(q.Pop(out));
    EXPECT_EQ(out, i);
  }
  int unused = 0;
  EXPECT_FALSE(q.Pop(unused));
}

TEST(MpscQueueTest, RefusesWhenFullAndRecoversAfterDraining) {
  MpscQueue<int> q(4);
  for (int i = 0; i < 4; i++) {
    ASSERT_TRUE(q.Push(i));
  }
  EXPECT_FALSE(q.Push(99)) << "a full ring is itself the bound";

  int out = 0;
  ASSERT_TRUE(q.Pop(out));
  EXPECT_TRUE(q.Push(99)) << "one slot freed, one push accepted";
}

TEST(MpscQueueTest, ReportsDepthAheadOfAPush) {
  MpscQueue<int> q(16);
  size_t queued = 999;
  ASSERT_TRUE(q.Push(1, &queued));
  EXPECT_EQ(queued, 0u) << "first into an empty queue";
  ASSERT_TRUE(q.Push(2, &queued));
  EXPECT_EQ(queued, 1u);
  ASSERT_TRUE(q.Push(3, &queued));
  EXPECT_EQ(queued, 2u);
}

TEST(MpscQueueTest, EmptyTracksContents) {
  MpscQueue<int> q(8);
  EXPECT_TRUE(q.Empty());
  ASSERT_TRUE(q.Push(1));
  EXPECT_FALSE(q.Empty());
  int out = 0;
  ASSERT_TRUE(q.Pop(out));
  EXPECT_TRUE(q.Empty());
}

TEST(MpscQueueTest, DrainToTakesEverythingOldestFirst) {
  MpscQueue<int> q(16);
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(q.Push(i));
  }
  std::vector<int> out;
  EXPECT_EQ(q.DrainTo(out), 5u);
  ASSERT_EQ(out.size(), 5u);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(out[static_cast<size_t>(i)], i);
  }
  EXPECT_TRUE(q.Empty());
}

TEST(MpscQueueTest, MovesOutOfTheSlotSoResourcesAreReleased) {
  MpscQueue<std::shared_ptr<int>> q(4);
  auto owned = std::make_shared<int>(7);
  ASSERT_TRUE(q.Push(owned));
  EXPECT_EQ(owned.use_count(), 2) << "queue holds one";

  std::shared_ptr<int> out;
  ASSERT_TRUE(q.Pop(out));
  out.reset();
  EXPECT_EQ(owned.use_count(), 1)
      << "the slot must not keep a copy alive until it is written a lap later";
}

// The structure the whole send path rests on, and previously untested.
TEST(MpscQueueTest, ManyProducersLoseNothing) {
  constexpr int kThreads = 4;
  constexpr int kPer = 2000;
  MpscQueue<int> q(1 << 14);
  std::atomic<int> refused{0};

  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; t++) {
    producers.emplace_back([&, t] {
      for (int i = 0; i < kPer; i++) {
        while (!q.Push(t * kPer + i)) {
          refused++;
          std::this_thread::yield();
        }
      }
    });
  }

  std::set<int> seen;
  while (static_cast<int>(seen.size()) < kThreads * kPer) {
    int v = 0;
    if (q.Pop(v)) {
      EXPECT_TRUE(seen.insert(v).second) << "duplicate value " << v;
    } else {
      std::this_thread::yield();
    }
  }
  for (auto& p : producers) {
    p.join();
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kThreads * kPer));
}

// --- OutboundQueue ------------------------------------------------------------

namespace {

std::shared_ptr<Packet> AnyPacket() {
  // the queue never inspects the packet, so a null shared_ptr is a fine stand-in
  return std::shared_ptr<Packet>{};
}

}  // namespace

TEST(OutboundQueueTest, WakesOnTheIdleEdgeOnly) {
  OutboundQueue q(16);
  int wakes = 0;
  q.SetWakeCallback([&] { wakes++; });

  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  EXPECT_EQ(wakes, 1) << "first push into an empty queue wakes the encoder";
  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  EXPECT_EQ(wakes, 1) << "a drain is already on its way; further wakes are waste";
}

TEST(OutboundQueueTest, RefusesWhenFull) {
  OutboundQueue q(2);
  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  EXPECT_FALSE(q.Push(AnyPacket(), {}))
      << "refusal is the backpressure signal";
  EXPECT_EQ(q.size(), 2u);
}

TEST(OutboundQueueTest, DrainEncodesEverythingQueued) {
  OutboundQueue q(16);
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(q.Push(AnyPacket(), {}));
  }
  int encoded = 0;
  EXPECT_TRUE(q.Drain([&](OutboundQueue::Item&) { encoded++; return true; }));
  EXPECT_EQ(encoded, 5);
  EXPECT_EQ(q.size(), 0u);
}

TEST(OutboundQueueTest, DrainOnAnEmptyQueueReportsNothingEncoded) {
  OutboundQueue q(16);
  EXPECT_FALSE(q.Drain([](OutboundQueue::Item&) { return true; }));
}

// A dead session still has to release what it queued rather than hold it until
// destruction, so draining continues even when nothing is encoded.
TEST(OutboundQueueTest, DrainsEvenWhenTheCallbackEncodesNothing) {
  OutboundQueue q(16);
  for (int i = 0; i < 4; i++) {
    ASSERT_TRUE(q.Push(AnyPacket(), {}));
  }
  int seen = 0;
  EXPECT_FALSE(q.Drain([&](OutboundQueue::Item&) { seen++; return false; }))
      << "nothing encoded, so nothing to flush";
  EXPECT_EQ(seen, 4) << "but every item was taken off the queue";
  EXPECT_EQ(q.size(), 0u);
}

// The claim is what keeps message order defined while letting any thread
// encode. A second entrant must decline rather than interleave.
TEST(OutboundQueueTest, ClaimIsExclusiveAndNonBlocking) {
  OutboundQueue q(16);
  ASSERT_TRUE(q.Push(AnyPacket(), {}));

  bool reentered_ran = false;
  bool reentered_result = true;
  q.Drain([&](OutboundQueue::Item&) {
    // a second drain, from inside the first
    reentered_result = q.Drain([&](OutboundQueue::Item&) {
      reentered_ran = true;
      return true;
    });
    return true;
  });
  EXPECT_FALSE(reentered_result) << "the second caller must decline";
  EXPECT_FALSE(reentered_ran) << "and must not encode anything";
}

// --- the policy that fixed the bimodal throughput -----------------------------

TEST(OutboundQueuePolicyTest, WithoutAnEncoderTheWorkerAlwaysEncodes) {
  OutboundQueue q(64);
  EXPECT_TRUE(q.ShouldEncodeInline()) << "empty, and nobody else will do it";
  for (int i = 0; i < 20; i++) {
    ASSERT_TRUE(q.Push(AnyPacket(), {}));
  }
  EXPECT_TRUE(q.ShouldEncodeInline())
      << "deep, but there is no encoder to hand it to";
}

TEST(OutboundQueuePolicyTest, WithAnEncoderTheWorkerKeepsOnlyTheLatencyCase) {
  OutboundQueue q(64);
  q.SetHasDedicatedEncoder(true);

  EXPECT_TRUE(q.ShouldEncodeInline()) << "empty";
  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  EXPECT_TRUE(q.ShouldEncodeInline())
      << "a lone message: waking a thread costs more than the encode";

  ASSERT_TRUE(q.Push(AnyPacket(), {}));
  EXPECT_FALSE(q.ShouldEncodeInline())
      << "deeper than the latency case belongs to the encoder, so encoding "
         "overlaps the flush instead of serializing in front of it";
}

TEST(OutboundQueuePolicyTest, RevertsToInlineWhenTheEncoderGoesAway) {
  OutboundQueue q(64);
  q.SetHasDedicatedEncoder(true);
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(q.Push(AnyPacket(), {}));
  }
  ASSERT_FALSE(q.ShouldEncodeInline());

  q.SetHasDedicatedEncoder(false);
  EXPECT_TRUE(q.ShouldEncodeInline())
      << "on teardown the worker owns encoding again";
}

// --- Codec framing ------------------------------------------------------------

namespace {

class TinyPacket : public Packet {
 public:
  TinyPacket() : Packet(7) {}
  uint32_t value = 0xABCD1234u;
};

// writes into the buffer it was handed, which is the contract
class GoodSerializer : public PacketSerializer<TinyPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<TinyPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->value);
    return buffer;
  }
  std::shared_ptr<TinyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<TinyPacket>();
    packet->value = buffer->ReadInt<uint32_t>();
    return packet;
  }
};

// hands back bytes it already had rather than writing them through again
class ReplacingSerializer : public PacketSerializer<TinyPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<TinyPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    (void)buffer;
    auto own = std::make_shared<Buffer>();
    own->WriteInt<uint32_t>(packet->value);
    return own;
  }
  std::shared_ptr<TinyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    (void)buffer;
    return nullptr;
  }
};

class RefusingSerializer : public PacketSerializer<TinyPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<TinyPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    (void)packet;
    (void)buffer;
    return nullptr;
  }
  std::shared_ptr<TinyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    (void)buffer;
    return nullptr;
  }
};

}  // namespace

TEST(CodecFramingTest, WritesTheDeclaredLengthOfWhatTheSerializerWrote) {
  Codec codec;
  codec.Add(7, std::make_unique<GoodSerializer>());

  auto buffer = codec.Serialize(std::make_shared<TinyPacket>(), 0);
  ASSERT_TRUE(buffer) << "the serializer honored the contract";

  EXPECT_EQ(buffer->ReadVarInt<PacketId>(), 7u);
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), sizeof(uint32_t))
      << "the placeholder was backfilled with the body length";
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 0xABCD1234u);
}

TEST(CodecFramingTest, FramesABodyTheSerializerBuiltItself) {
  Codec codec;
  codec.Add(7, std::make_unique<ReplacingSerializer>());

  auto buffer = codec.Serialize(std::make_shared<TinyPacket>(), 0);
  ASSERT_TRUE(buffer);

  // the header stays in the codec's buffer and the body is copied in behind it,
  // so the frame is indistinguishable from one written in place
  EXPECT_EQ(buffer->ReadVarInt<PacketId>(), 7u);
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), sizeof(uint32_t));
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 0xABCD1234u);
}

TEST(CodecFramingTest, FramesTheSameBytesEitherWayTheSerializerWorks) {
  Codec in_place;
  in_place.Add(7, std::make_unique<GoodSerializer>());
  Codec own_buffer;
  own_buffer.Add(7, std::make_unique<ReplacingSerializer>());

  auto a = in_place.Serialize(std::make_shared<TinyPacket>(), 0);
  auto b = own_buffer.Serialize(std::make_shared<TinyPacket>(), 0);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_EQ(a->readable_bytes(), b->readable_bytes());
  EXPECT_EQ(std::string(a->read_cursor_data(), a->readable_bytes()),
            std::string(b->read_cursor_data(), b->readable_bytes()))
      << "which path produced the body must not be visible on the wire";
}

TEST(CodecFramingTest, DropsThePacketWhenTheSerializerRefuses) {
  Codec codec;
  codec.Add(7, std::make_unique<RefusingSerializer>());
  EXPECT_FALSE(codec.Serialize(std::make_shared<TinyPacket>(), 0));
}

TEST(CodecFramingTest, DropsThePacketWhenNoSerializerIsRegistered) {
  Codec codec;
  EXPECT_FALSE(codec.Serialize(std::make_shared<TinyPacket>(), 0));
}

// --- Codec decode failures ----------------------------------------------------

namespace {

class OtherPacket : public Packet {
 public:
  OtherPacket() : Packet(8) {}
  uint32_t value = 0x11223344u;
};

class OtherSerializer : public PacketSerializer<OtherPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<OtherPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->value);
    return buffer;
  }
  std::shared_ptr<OtherPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<OtherPacket>();
    packet->value = buffer->ReadInt<uint32_t>();
    return packet;
  }
};

// walks the cursor out of its frame by hand: the read limit only fences reads,
// which is exactly the misbehaviour the over-read branch exists to catch
class OverreadingSerializer : public PacketSerializer<TinyPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<TinyPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    (void)packet;
    return buffer;
  }
  std::shared_ptr<TinyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    buffer->SkipRead(sizeof(uint32_t) + 2);
    return std::make_shared<TinyPacket>();
  }
};

class CountingHandler : public PacketHandlerBase {
 public:
  void Handle(std::shared_ptr<Packet> packet) override {
    (void)packet;
    handled++;
  }
  int handled = 0;
};

std::shared_ptr<Buffer> TinyFrame() {
  Codec codec;
  codec.Add(7, std::make_unique<GoodSerializer>());
  return codec.Serialize(std::make_shared<TinyPacket>(), 0);
}

std::shared_ptr<Buffer> OtherFrame() {
  Codec codec;
  codec.Add(8, std::make_unique<OtherSerializer>());
  return codec.Serialize(std::make_shared<OtherPacket>(), 0);
}

std::shared_ptr<Buffer> Concat(std::initializer_list<std::shared_ptr<Buffer>> frames) {
  auto out = std::make_shared<Buffer>();
  for (const auto& frame : frames) {
    out->Write(frame->read_cursor_data(), frame->readable_bytes());
  }
  return out;
}

}  // namespace

TEST(CodecDecodeTest, CleanFramesCountNothing) {
  Codec codec;
  codec.Add(7, std::make_unique<GoodSerializer>());
  CountingHandler handler;

  DecodeStats stats =
      codec.Deserialize(Concat({TinyFrame(), TinyFrame()}), handler);
  EXPECT_EQ(handler.handled, 2);
  EXPECT_EQ(stats.invalid_frames, 0u);
  EXPECT_FALSE(stats.framing_lost);
}

TEST(CodecDecodeTest, RefusedFrameIsCountedAndTheNextOneStillDecodes) {
  Codec codec;
  codec.Add(7, std::make_unique<RefusingSerializer>());
  codec.Add(8, std::make_unique<OtherSerializer>());
  CountingHandler handler;

  DecodeStats stats =
      codec.Deserialize(Concat({TinyFrame(), OtherFrame()}), handler);
  EXPECT_EQ(handler.handled, 1) << "the refused frame was skipped, not fatal";
  EXPECT_EQ(stats.invalid_frames, 1u);
  EXPECT_FALSE(stats.framing_lost);
}

TEST(CodecDecodeTest, UnknownIdSkipsWithoutCounting) {
  Codec codec;
  codec.Add(8, std::make_unique<OtherSerializer>());
  CountingHandler handler;

  DecodeStats stats =
      codec.Deserialize(Concat({TinyFrame(), OtherFrame()}), handler);
  EXPECT_EQ(handler.handled, 1);
  EXPECT_EQ(stats.invalid_frames, 0u) << "version skew is not an offense";
  EXPECT_FALSE(stats.framing_lost);
}

TEST(CodecDecodeTest, OverreadDropsTheRestAndLosesFraming) {
  Codec codec;
  codec.Add(7, std::make_unique<OverreadingSerializer>());
  codec.Add(8, std::make_unique<OtherSerializer>());
  CountingHandler handler;

  DecodeStats stats =
      codec.Deserialize(Concat({TinyFrame(), OtherFrame()}), handler);
  EXPECT_EQ(handler.handled, 0) << "nothing after the overrun can be located";
  EXPECT_EQ(stats.invalid_frames, 1u);
  EXPECT_TRUE(stats.framing_lost);
}

TEST(CodecDecodeTest, DeclaredSizeBeyondTheBufferLosesFraming) {
  Codec codec;
  codec.Add(7, std::make_unique<GoodSerializer>());
  CountingHandler handler;

  auto buffer = Concat({OtherFrame()});  // one healthy unknown-id frame first
  buffer->WriteVarInt<PacketId>(7);
  buffer->WriteInt<uint32_t>(100);  // a length nothing here can back

  DecodeStats stats = codec.Deserialize(buffer, handler);
  EXPECT_EQ(stats.invalid_frames, 1u);
  EXPECT_TRUE(stats.framing_lost);
}

TEST(CodecDecodeTest, GarbageHeaderLosesFraming) {
  Codec codec;
  codec.Add(7, std::make_unique<GoodSerializer>());
  CountingHandler handler;

  auto buffer = std::make_shared<Buffer>();
  buffer->WriteInt<uint8_t>(7);  // a valid id varint, then a truncated size
  buffer->WriteInt<uint8_t>(1);

  DecodeStats stats = codec.Deserialize(buffer, handler);
  EXPECT_EQ(handler.handled, 0);
  EXPECT_EQ(stats.invalid_frames, 1u);
  EXPECT_TRUE(stats.framing_lost);
}

TEST(CodecDecodeTest, DumpWritesTheEvidenceToTheLog) {
  Codec codec;
  codec.Add(7, std::make_unique<OverreadingSerializer>());
  CountingHandler handler;

  std::ostringstream captured;
  SetLogStream(captured);
  DecodeStats stats =
      codec.Deserialize(Concat({TinyFrame()}), handler, /*dump_on_failure=*/true);
  SetLogStream(std::cout);

  EXPECT_EQ(stats.invalid_frames, 1u);
  EXPECT_NE(captured.str().find("Undecodable frame at offset"), std::string::npos)
      << "the dump names itself so it can be found in a log";
}

// --- Invalid-frame threshold over a session -----------------------------------

namespace {

// the same misbehaviour as OverreadingSerializer, on the probe packet the
// session pair speaks
class OverreadingProbeSerializer : public PacketSerializer<ProbePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ProbePacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    (void)packet;
    return buffer;
  }
  std::shared_ptr<ProbePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    buffer->SkipRead(sizeof(uint32_t) + 2);
    return std::make_shared<ProbePacket>();
  }
};

std::shared_ptr<Codec> MakeOverreadingCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketProbe, std::make_unique<OverreadingProbeSerializer>());
  return codec;
}

}  // namespace

TEST(InvalidFrameThreshold, ClosesTheSessionAtTheLimit) {
  ASSERT_EQ(Init(), Result::Success);
  SessionOptions base;
  base.common.max_invalid_frames = 3;
  Pair pair(/*encryption=*/true, base);
  ASSERT_TRUE(pair.Handshake());

  // every probe the client sends arrives as one undecodable frame
  pair.server->SetCodec(MakeOverreadingCodec());

  for (uint32_t i = 0; i < 3; i++) {
    EXPECT_TRUE(pair.server->IsAlive());
    pair.Deliver(pair.Emit(i, 0));
  }
  EXPECT_FALSE(pair.server->IsAlive()) << "the third bad frame is the limit";
  EXPECT_EQ(pair.server->invalid_frames(), 3u);
}

TEST(InvalidFrameThreshold, ZeroDisablesTheClose) {
  ASSERT_EQ(Init(), Result::Success);
  SessionOptions base;
  base.common.max_invalid_frames = 0;
  Pair pair(/*encryption=*/true, base);
  ASSERT_TRUE(pair.Handshake());

  pair.server->SetCodec(MakeOverreadingCodec());

  for (uint32_t i = 0; i < 5; i++) {
    pair.Deliver(pair.Emit(i, 0));
  }
  EXPECT_TRUE(pair.server->IsAlive()) << "counted, never acted on";
  EXPECT_EQ(pair.server->invalid_frames(), 5u);
}

// The four byte counters mean two different things on purpose: message_bytes_*
// is what crossed the wire, payload_bytes_* is the serialized packet. Encryption
// makes the two differ by a fixed overhead, which is what pins them apart here.
TEST(ByteMetrics, WireAndPayloadAreCountedSeparately) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair(/*encryption=*/true);
  ASSERT_TRUE(pair.Handshake());

  const auto before_sent = pair.client->metrics().common;
  const auto before_recv = pair.server->metrics().common;
  pair.Deliver(pair.Emit(7, 0));

  const auto sent = pair.client->metrics().common;
  const auto recv = pair.server->metrics().common;

  const uint64_t wire_out = sent.message_bytes_sent - before_sent.message_bytes_sent;
  const uint64_t body_out = sent.payload_bytes_sent - before_sent.payload_bytes_sent;
  const uint64_t wire_in = recv.message_bytes_received - before_recv.message_bytes_received;
  const uint64_t body_in = recv.payload_bytes_received - before_recv.payload_bytes_received;

  EXPECT_GT(body_out, 0u);
  EXPECT_GT(wire_out, body_out) << "encryption only ever adds bytes";
  // the two ends measure the same message at the same two stages
  EXPECT_EQ(wire_in, wire_out) << "received wire bytes must match what was sent";
  EXPECT_EQ(body_in, body_out) << "received payload bytes must match what was serialized";
}

// Without a codec nothing can be dispatched, but the bytes still arrived, and a
// bandwidth counter that hides them is worse than useless.
TEST(ByteMetrics, WireBytesAreCountedWithoutACodec) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair(/*encryption=*/true);
  ASSERT_TRUE(pair.Handshake());

  auto frame = pair.Emit(9, 0);
  pair.server->SetHandler(nullptr);
  const auto before = pair.server->metrics().common;
  pair.Deliver(frame);
  const auto after = pair.server->metrics().common;

  EXPECT_GT(after.message_bytes_received, before.message_bytes_received)
      << "wire bytes are counted even when nothing dispatches them";
  EXPECT_EQ(after.payload_bytes_received, before.payload_bytes_received)
      << "payload bytes only count what reached a handler";
}
