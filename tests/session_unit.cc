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
// underneath it, the arbitration built on top, and the codec's framing. All are
// reachable only through a live connection otherwise, which is why the race
// that made throughput bimodal showed up as a benchmark artifact rather than a
// failure, and why a serializer breaking the frame header went unnoticed.
//

#include "znet/codec.h"
#include "znet/mpsc_queue.h"
#include "znet/outbound_queue.h"
#include "znet/packet_serializer.h"

#include <gtest/gtest.h>

#include <atomic>
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
  EXPECT_EQ(buffer->ReadInt<size_t>(), sizeof(uint32_t))
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
  EXPECT_EQ(buffer->ReadInt<size_t>(), sizeof(uint32_t));
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
