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
// Unit tests for ZDT's protocol components in isolation: no sockets, no
// threads, no timers, so they run in milliseconds and a failure names one
// component rather than "the connection misbehaved". The socket-level and
// end-to-end tests live in zdt.cc.
//

#include "znet/backends/zdt/zdt_ack_history.h"
#include "znet/backends/zdt/zdt_congestion.h"
#include "znet/backends/zdt/zdt_wire.h"

#include <gtest/gtest.h>

using namespace znet;
using namespace znet::backends;

namespace {

// how far back Fill() is allowed to describe; wide enough not to clamp unless
// a test is specifically about clamping
constexpr size_t kWideCap = kZDTAckHistoryBits;

ZDTHeader FillFrom(const ZDTAckHistory& history, size_t max_blocks = kZDTMaxAckBlocks,
                   size_t cap = kWideCap) {
  ZDTHeader header;
  history.Fill(header, max_blocks, cap);
  return header;
}

}  // namespace

// --- sequence comparison ------------------------------------------------------

// These decide what counts as "newer" everywhere in the protocol, and they have
// to keep working across the 16-bit wrap that happens every 65536 datagrams.
TEST(ZDTSeqCompare, OrdersNormallyAwayFromTheWrap) {
  EXPECT_TRUE(SeqGreater(5, 4));
  EXPECT_FALSE(SeqGreater(4, 5));
  EXPECT_TRUE(SeqLess(4, 5));
  EXPECT_FALSE(SeqLess(5, 4));
}

TEST(ZDTSeqCompare, TreatsWraparoundAsContinuing) {
  EXPECT_TRUE(SeqGreater(0, 65535)) << "0 follows 65535, it does not precede it";
  EXPECT_TRUE(SeqLess(65535, 0));
  EXPECT_TRUE(SeqGreater(1, 65534));
}

TEST(ZDTSeqCompare, EqualIsNeitherGreaterNorLess) {
  EXPECT_FALSE(SeqGreater(7, 7));
  EXPECT_FALSE(SeqLess(7, 7));
}

// --- ack history --------------------------------------------------------------

TEST(ZDTAckHistoryTest, EmptyHistoryReportsNothing) {
  ZDTAckHistory history;
  EXPECT_FALSE(history.has_any());
  ZDTHeader header = FillFrom(history);
  EXPECT_EQ(header.block_count, 0);
}

TEST(ZDTAckHistoryTest, ContiguousRunIsOneBlock) {
  ZDTAckHistory history;
  for (WireSeq s = 10; s <= 12; s++) {
    history.Record(s);
  }
  EXPECT_TRUE(history.has_any());
  EXPECT_EQ(history.highest(), 12);

  ZDTHeader header = FillFrom(history);
  ASSERT_EQ(header.block_count, 1);
  EXPECT_EQ(header.ack, 12);
  EXPECT_EQ(header.blocks[0].num_ack, 3);
  EXPECT_EQ(header.blocks[0].num_nack, 0);
}

// The gap is the whole point of the encoding: it is what tells the sender to
// retransmit, so it must survive as a nack run rather than being folded into a
// neighbouring ack run.
TEST(ZDTAckHistoryTest, GapBecomesANackRun) {
  ZDTAckHistory history;
  history.Record(10);
  history.Record(12);  // 11 never arrived

  ZDTHeader header = FillFrom(history);
  EXPECT_EQ(header.ack, 12);
  ASSERT_EQ(header.block_count, 2);
  EXPECT_EQ(header.blocks[0].num_ack, 1);   // 12
  EXPECT_EQ(header.blocks[0].num_nack, 1);  // 11
  EXPECT_EQ(header.blocks[1].num_ack, 1);   // 10
  EXPECT_EQ(header.blocks[1].num_nack, 0);
}

// A fresh connection has seen one datagram and knows nothing about the 1023
// sequences before it. Reporting those as missing would invent losses and make
// the peer retransmit traffic it never sent.
TEST(ZDTAckHistoryTest, FreshConnectionDoesNotInventLosses) {
  ZDTAckHistory history;
  history.Record(5000);

  ZDTHeader header = FillFrom(history);
  ASSERT_EQ(header.block_count, 1);
  EXPECT_EQ(header.blocks[0].num_ack, 1);
  EXPECT_EQ(header.blocks[0].num_nack, 0)
      << "history never observed must not be encoded as missing";
}

TEST(ZDTAckHistoryTest, LateArrivalFillsItsGapIn) {
  ZDTAckHistory history;
  history.Record(10);
  history.Record(12);
  history.Record(11);  // arrives out of order

  ZDTHeader header = FillFrom(history);
  ASSERT_EQ(header.block_count, 1) << "the gap closed, so one run covers it";
  EXPECT_EQ(header.blocks[0].num_ack, 3);
  EXPECT_EQ(header.blocks[0].num_nack, 0);
}

TEST(ZDTAckHistoryTest, DuplicateOfHighestChangesNothing) {
  ZDTAckHistory history;
  history.Record(20);
  history.Record(21);
  ZDTHeader before = FillFrom(history);
  history.Record(21);
  ZDTHeader after = FillFrom(history);

  EXPECT_EQ(after.ack, before.ack);
  ASSERT_EQ(after.block_count, before.block_count);
  EXPECT_EQ(after.blocks[0].num_ack, before.blocks[0].num_ack);
}

// "Everything older than this is still missing" tells the sender nothing it can
// act on, and a later ack will carry those sequences once something in between
// lands. So a trailing block with no acks is dropped.
TEST(ZDTAckHistoryTest, TrailingAllNackBlockIsTrimmed) {
  ZDTAckHistory history;
  history.Record(100);
  history.Record(105);  // 101-104 missing, and they are the oldest thing known

  ZDTHeader header = FillFrom(history);
  ASSERT_GT(header.block_count, 0);
  EXPECT_GT(header.blocks[header.block_count - 1].num_ack, 0)
      << "the last block must not be nack-only";
}

TEST(ZDTAckHistoryTest, RespectsMaxBlocks) {
  ZDTAckHistory history;
  // alternate arrived/missing so every pair forces a new block
  for (WireSeq s = 0; s < 40; s += 2) {
    history.Record(s);
  }
  ZDTHeader header = FillFrom(history, /*max_blocks=*/3);
  EXPECT_LE(header.block_count, 3);
}

TEST(ZDTAckHistoryTest, MaxBlocksZeroStillReportsTheAck) {
  ZDTAckHistory history;
  history.Record(77);
  ZDTHeader header = FillFrom(history, /*max_blocks=*/0);
  EXPECT_EQ(header.ack, 77) << "the peer still needs the highest sequence";
  EXPECT_EQ(header.block_count, 0);
}

TEST(ZDTAckHistoryTest, ClampsToMaxAckBlocks) {
  ZDTAckHistory history;
  for (WireSeq s = 0; s < 200; s += 2) {
    history.Record(s);
  }
  ZDTHeader header = FillFrom(history, /*max_blocks=*/kZDTMaxAckBlocks * 4);
  EXPECT_LE(header.block_count, kZDTMaxAckBlocks)
      << "a caller asking for more than the wire format holds must be clamped";
}

// The cap is how far back is worth describing at all: older than the send
// window, the peer has already seen those acked or it could not have kept
// sending.
TEST(ZDTAckHistoryTest, ReportableCapLimitsHowFarBackItLooks) {
  ZDTAckHistory history;
  for (WireSeq s = 0; s < 100; s++) {
    if (s != 50) {
      history.Record(s);  // one gap, 49 back from the newest
    }
  }
  ZDTHeader wide = FillFrom(history, kZDTMaxAckBlocks, /*cap=*/kWideCap);
  ZDTHeader narrow = FillFrom(history, kZDTMaxAckBlocks, /*cap=*/10);

  EXPECT_EQ(wide.block_count, 2) << "the gap at 50 is inside a wide cap";
  EXPECT_EQ(narrow.block_count, 1) << "a cap of 10 cannot see back to 50";
  EXPECT_EQ(narrow.blocks[0].num_ack, 10);
}

TEST(ZDTAckHistoryTest, SurvivesSequenceWraparound) {
  ZDTAckHistory history;
  history.Record(65534);
  history.Record(65535);
  history.Record(0);  // wraps
  history.Record(1);

  EXPECT_EQ(history.highest(), 1);
  ZDTHeader header = FillFrom(history);
  EXPECT_EQ(header.ack, 1);
  ASSERT_EQ(header.block_count, 1);
  EXPECT_EQ(header.blocks[0].num_ack, 4)
      << "the run must stay contiguous across the wrap";
}

// A jump larger than the window discards everything, rather than shifting
// stale bits into positions they no longer describe.
TEST(ZDTAckHistoryTest, HugeJumpResetsRatherThanMisreporting) {
  ZDTAckHistory history;
  history.Record(1);
  history.Record(2);
  history.Record(static_cast<WireSeq>(kZDTAckHistoryBits + 500));

  ZDTHeader header = FillFrom(history);
  ASSERT_GE(header.block_count, 1);
  EXPECT_EQ(header.blocks[0].num_ack, 1) << "only the newest is known to have arrived";
}

// --- RTT estimator ------------------------------------------------------------

namespace {

using Ms = std::chrono::milliseconds;
using TP = std::chrono::steady_clock::time_point;

TP At(long long ms) { return TP{} + Ms{ms}; }

}  // namespace

TEST(ZDTRttTest, FirstSampleSeedsEverything) {
  ZDTRttEstimator rtt;
  EXPECT_FALSE(rtt.has_rtt());
  rtt.OnSample(Ms{40}, At(0), Ms{10}, Ms{2000});

  EXPECT_TRUE(rtt.has_rtt());
  EXPECT_DOUBLE_EQ(rtt.srtt_ms(), 40.0);
  EXPECT_DOUBLE_EQ(rtt.rtt_min_ms(), 40.0);
  // srtt + 4*rttvar, and the first sample seeds rttvar at half the sample
  EXPECT_EQ(rtt.rto(), Ms{120});
}

TEST(ZDTRttTest, NegativeSampleIsIgnored) {
  ZDTRttEstimator rtt;
  rtt.OnSample(Ms{-5}, At(0), Ms{10}, Ms{2000});
  EXPECT_FALSE(rtt.has_rtt()) << "a negative round trip is a clock artifact";
}

TEST(ZDTRttTest, RtoIsClampedBothWays) {
  ZDTRttEstimator low;
  low.OnSample(Ms{1}, At(0), Ms{100}, Ms{2000});
  EXPECT_EQ(low.rto(), Ms{100}) << "must not drop below rto_min";

  ZDTRttEstimator high;
  high.OnSample(Ms{5000}, At(0), Ms{100}, Ms{2000});
  EXPECT_EQ(high.rto(), Ms{2000}) << "must not exceed rto_max";
}

TEST(ZDTRttTest, MinimumTracksDownwardImmediately) {
  ZDTRttEstimator rtt;
  rtt.OnSample(Ms{50}, At(0), Ms{10}, Ms{2000});
  rtt.OnSample(Ms{20}, At(10), Ms{10}, Ms{2000});
  EXPECT_DOUBLE_EQ(rtt.rtt_min_ms(), 20.0);
}

// The floor is windowed, not lifetime: a route change that raises the real
// baseline would otherwise read as permanent queueing and pin the window.
TEST(ZDTRttTest, MinimumIsReprobedOnceStale) {
  ZDTRttEstimator rtt;
  rtt.OnSample(Ms{20}, At(0), Ms{10}, Ms{2000});
  ASSERT_DOUBLE_EQ(rtt.rtt_min_ms(), 20.0);

  // still inside the window: a larger sample must not raise the floor
  rtt.OnSample(Ms{80}, At(kZDTRttMinWindowMs / 2), Ms{10}, Ms{2000});
  EXPECT_DOUBLE_EQ(rtt.rtt_min_ms(), 20.0);

  // past it: the floor is re-probed from the current sample
  rtt.OnSample(Ms{80}, At(kZDTRttMinWindowMs + 1), Ms{10}, Ms{2000});
  EXPECT_DOUBLE_EQ(rtt.rtt_min_ms(), 80.0);
}

TEST(ZDTRttTest, QueueingIsTheRatioAgainstTheFloor) {
  ZDTRttEstimator rtt;
  rtt.OnSample(Ms{100}, At(0), Ms{10}, Ms{5000});
  EXPECT_FALSE(rtt.IsQueueing()) << "a single sample is its own floor";

  // drive srtt up while the floor stays at 100
  for (int i = 0; i < 40; i++) {
    rtt.OnSample(Ms{200}, At(10 + i), Ms{10}, Ms{5000});
  }
  EXPECT_GT(rtt.srtt_ms(), rtt.rtt_min_ms() * kZDTQueueingRttRatio);
  EXPECT_TRUE(rtt.IsQueueing());
}

TEST(ZDTRttTest, NoSamplesMeansNoQueueingSignal) {
  ZDTRttEstimator rtt;
  EXPECT_FALSE(rtt.IsQueueing()) << "nothing measured yet is not congestion";
}

// --- congestion controller ----------------------------------------------------

namespace {

// an estimator parked in a chosen state, so window tests can choose whether the
// queueing signal is asserted without simulating a link
ZDTRttEstimator QuietRtt() {
  ZDTRttEstimator rtt;
  rtt.OnSample(Ms{100}, At(0), Ms{10}, Ms{5000});
  return rtt;
}

ZDTRttEstimator QueueingRtt() {
  ZDTRttEstimator rtt;
  rtt.OnSample(Ms{10}, At(0), Ms{1}, Ms{5000});
  for (int i = 0; i < 60; i++) {
    rtt.OnSample(Ms{100}, At(1 + i), Ms{1}, Ms{5000});
  }
  return rtt;
}

constexpr int kCap = 512;

}  // namespace

TEST(ZDTCongestionTest, StartsAtInitialWindowTen) {
  ZDTCongestionController cc;
  EXPECT_DOUBLE_EQ(cc.cwnd(), 10.0);
  EXPECT_EQ(cc.Window(kCap), 10);
}

TEST(ZDTCongestionTest, SlowStartGrowsOnePerAckedDatagram) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QuietRtt();
  cc.OnAcked(3, rtt, /*next_seq=*/1, kCap);
  EXPECT_DOUBLE_EQ(cc.cwnd(), 13.0);
}

TEST(ZDTCongestionTest, WindowIsClampedToCap) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QuietRtt();
  for (int i = 0; i < 200; i++) {
    cc.OnAcked(50, rtt, /*next_seq=*/1, kCap);
  }
  EXPECT_DOUBLE_EQ(cc.cwnd(), static_cast<double>(kCap));
  EXPECT_EQ(cc.Window(kCap), kCap);
  EXPECT_EQ(cc.Window(16), 16) << "a smaller cap still applies";
}

TEST(ZDTCongestionTest, WindowNeverStallsBelowTwo) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QueueingRtt();
  for (int i = 0; i < 200; i++) {
    cc.OnAcked(1, rtt, /*next_seq=*/1, kCap);
    cc.OnAckArrived(1);  // close the epoch so the next ack can back off again
  }
  EXPECT_GE(cc.Window(kCap), 2) << "forward progress must always be possible";
}

TEST(ZDTCongestionTest, ZeroAckedIsANoOp) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QuietRtt();
  cc.OnAcked(0, rtt, 1, kCap);
  EXPECT_DOUBLE_EQ(cc.cwnd(), 10.0);
}

// One loss event should cost one reduction, not one per datagram in it.
TEST(ZDTCongestionTest, QueueingBacksOffOncePerEpoch) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QueueingRtt();
  const double before = cc.cwnd();

  cc.OnAcked(1, rtt, /*next_seq=*/100, kCap);
  const double after_first = cc.cwnd();
  EXPECT_LT(after_first, before);
  EXPECT_TRUE(cc.in_loss_recovery());

  cc.OnAcked(1, rtt, /*next_seq=*/101, kCap);
  EXPECT_DOUBLE_EQ(cc.cwnd(), after_first) << "still the same epoch";
}

TEST(ZDTCongestionTest, RecoveryEndsOnceTheEpochSequenceIsAcked) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QueueingRtt();
  cc.OnAcked(1, rtt, /*next_seq=*/100, kCap);
  ASSERT_TRUE(cc.in_loss_recovery());

  cc.OnAckArrived(99);
  EXPECT_TRUE(cc.in_loss_recovery()) << "older than the epoch changes nothing";
  cc.OnAckArrived(100);
  EXPECT_FALSE(cc.in_loss_recovery());
}

TEST(ZDTCongestionTest, QueueingTimeoutCollapsesToTwoOncePerEpoch) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QueueingRtt();
  cc.OnRetransmitTimeout(rtt, /*next_seq=*/50);
  EXPECT_DOUBLE_EQ(cc.cwnd(), 2.0);

  cc.OnRetransmitTimeout(rtt, /*next_seq=*/51);
  EXPECT_DOUBLE_EQ(cc.cwnd(), 2.0) << "same epoch, no second collapse";
}

// CHARACTERIZATION, not endorsement. On a lossy-but-uncongested path the
// queueing signal is false, and this branch carries no epoch guard, so every
// scan that finds a timeout multiplies the window again. Several scans can fall
// inside one round trip, which walks the window down to its floor and keeps it
// there. This asserts what the code does today; see the refactor plan.
TEST(ZDTCongestionTest, RepeatedTimeoutsWithoutQueueingWalkDownToTheFloor) {
  ZDTCongestionController cc;
  const ZDTRttEstimator rtt = QuietRtt();
  ASSERT_FALSE(rtt.IsQueueing());

  const double start = cc.cwnd();
  cc.OnRetransmitTimeout(rtt, /*next_seq=*/10);
  EXPECT_DOUBLE_EQ(cc.cwnd(), start * 0.9) << "one reduction per scan";

  for (int i = 0; i < 40; i++) {
    cc.OnRetransmitTimeout(rtt, static_cast<WireSeq>(11 + i));
  }
  EXPECT_DOUBLE_EQ(cc.cwnd(), 8.0)
      << "unguarded by any epoch, repeated scans reach the floor";
  EXPECT_FALSE(cc.in_loss_recovery())
      << "and this path never opens an epoch, so nothing suppresses the next one";
}

// The two timeout paths floor at different values, which is worth pinning
// because it is surprising rather than obviously intended.
TEST(ZDTCongestionTest, TheTwoTimeoutFloorsDisagree) {
  ZDTCongestionController queueing;
  queueing.OnRetransmitTimeout(QueueingRtt(), 1);
  EXPECT_DOUBLE_EQ(queueing.cwnd(), 2.0);

  ZDTCongestionController lossy;
  const ZDTRttEstimator quiet = QuietRtt();
  for (int i = 0; i < 50; i++) {
    lossy.OnRetransmitTimeout(quiet, static_cast<WireSeq>(i));
  }
  EXPECT_DOUBLE_EQ(lossy.cwnd(), 8.0);
}
