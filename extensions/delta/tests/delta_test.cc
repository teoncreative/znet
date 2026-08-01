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

#include <cmath>
#include <random>
#include <vector>

#include "znet/ext/delta/delta.h"

using znet::Buffer;
using znet::Endianness;
using znet::ext::BitReader;
using znet::ext::BitWriter;
using znet::ext::DeltaReader;
using znet::ext::DeltaWriter;
using znet::ext::SequenceDifference;
using znet::ext::SequenceGreaterThan;
using znet::ext::SequenceLessThan;
using znet::ext::SnapshotHistory;

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

// a stand-in for a game's per-entity state.
struct PlayerState {
  uint64_t ammo = 0;
  int64_t health = 0;
  float yaw = 0.0f;
  float pitch = 0.0f;
  uint32_t weapon = 0;
  bool firing = false;

  bool operator==(const PlayerState& other) const {
    return ammo == other.ammo && health == other.health && yaw == other.yaw &&
           pitch == other.pitch && weapon == other.weapon &&
           firing == other.firing;
  }
};

const uint64_t kAmmoMin = 0;
const uint64_t kAmmoMax = 200;
const int64_t kHealthMin = -50;
const int64_t kHealthMax = 100;
const float kAngleMin = -3.15f;
const float kAngleMax = 3.15f;
const unsigned kAngleBits = 12;
const unsigned kWeaponBits = 4;

size_t WriteState(Buffer& buffer, const PlayerState& now,
                  const PlayerState& was) {
  BitWriter bits(buffer);
  DeltaWriter delta(bits);
  delta.WriteUIntRanged(now.ammo, was.ammo, kAmmoMin, kAmmoMax);
  delta.WriteIntRanged(now.health, was.health, kHealthMin, kHealthMax);
  delta.WriteFloatRanged(now.yaw, was.yaw, kAngleMin, kAngleMax, kAngleBits);
  delta.WriteFloatRanged(now.pitch, was.pitch, kAngleMin, kAngleMax,
                         kAngleBits);
  delta.WriteBits(now.weapon, was.weapon, kWeaponBits);
  delta.WriteBool(now.firing);
  return bits.bits_written();
}

PlayerState ReadState(Buffer& buffer, const PlayerState& was) {
  BitReader bits(buffer);
  DeltaReader delta(bits);
  PlayerState out;
  out.ammo = delta.ReadUIntRanged(was.ammo, kAmmoMin, kAmmoMax);
  out.health = delta.ReadIntRanged(was.health, kHealthMin, kHealthMax);
  out.yaw = delta.ReadFloatRanged(was.yaw, kAngleMin, kAngleMax, kAngleBits);
  out.pitch =
      delta.ReadFloatRanged(was.pitch, kAngleMin, kAngleMax, kAngleBits);
  out.weapon = delta.ReadBits(was.weapon, kWeaponBits);
  out.firing = delta.ReadBool();
  return out;
}

/** @brief The state as it survives one encode/decode, for comparing against. */
PlayerState Quantized(const PlayerState& state) {
  auto buffer = MakeBuffer();
  // force every field to be sent by deltaing against something that differs
  PlayerState different;
  different.ammo = state.ammo + 1;
  different.health = state.health + 1;
  different.yaw = state.yaw + 1.0f;
  different.pitch = state.pitch + 1.0f;
  different.weapon = state.weapon + 1;
  WriteState(*buffer, state, different);
  return ReadState(*buffer, different);
}

}  // namespace

// ---------------------------------------------------------------------------
// Sequence numbers
// ---------------------------------------------------------------------------

TEST(Sequence, OrdersOrdinaryValues) {
  EXPECT_TRUE(SequenceGreaterThan<uint16_t>(5, 4));
  EXPECT_FALSE(SequenceGreaterThan<uint16_t>(4, 5));
  EXPECT_TRUE(SequenceLessThan<uint16_t>(4, 5));
  EXPECT_FALSE(SequenceGreaterThan<uint16_t>(5, 5));
}

// the reason this exists: after a wrap, 0 is newer than 65535.
TEST(Sequence, SurvivesWrapAround) {
  EXPECT_TRUE(SequenceGreaterThan<uint16_t>(0, 65535));
  EXPECT_TRUE(SequenceGreaterThan<uint16_t>(1, 65535));
  EXPECT_TRUE(SequenceGreaterThan<uint16_t>(100, 65500));
  EXPECT_FALSE(SequenceGreaterThan<uint16_t>(65535, 0));
  EXPECT_TRUE(SequenceLessThan<uint16_t>(65535, 0));
}

TEST(Sequence, WorksForOtherWidths) {
  EXPECT_TRUE(SequenceGreaterThan<uint8_t>(0, 255));
  EXPECT_FALSE(SequenceGreaterThan<uint8_t>(255, 0));
  EXPECT_TRUE(SequenceGreaterThan<uint32_t>(0, 0xFFFFFFFFu));
  EXPECT_FALSE(SequenceGreaterThan<uint32_t>(0xFFFFFFFFu, 0));
}

// a counter stepped all the way round the space must stay consistently ordered
// at every point, including where it wraps.
TEST(Sequence, IsConsistentAcrossTheWholeSpace) {
  for (uint32_t i = 0; i < 65536u; ++i) {
    const uint16_t current = static_cast<uint16_t>(i);
    const uint16_t next = static_cast<uint16_t>(i + 1);
    const uint16_t previous = static_cast<uint16_t>(i - 1);
    ASSERT_TRUE(SequenceGreaterThan(next, current)) << "at " << i;
    ASSERT_TRUE(SequenceLessThan(previous, current)) << "at " << i;
  }
}

TEST(Sequence, DifferenceIsSignedAndWraps) {
  EXPECT_EQ(SequenceDifference<uint16_t>(10, 4), 6);
  EXPECT_EQ(SequenceDifference<uint16_t>(4, 10), -6);
  EXPECT_EQ(SequenceDifference<uint16_t>(0, 65535), 1);
  EXPECT_EQ(SequenceDifference<uint16_t>(65535, 0), -1);
  EXPECT_EQ(SequenceDifference<uint16_t>(7, 7), 0);
}

// ---------------------------------------------------------------------------
// Snapshot history
// ---------------------------------------------------------------------------

TEST(SnapshotHistory, StoresAndFinds) {
  SnapshotHistory<int, 8> history;
  history.Store(1, 100);
  history.Store(2, 200);

  ASSERT_NE(history.Find(1), nullptr);
  EXPECT_EQ(*history.Find(1), 100);
  ASSERT_NE(history.Find(2), nullptr);
  EXPECT_EQ(*history.Find(2), 200);
  EXPECT_EQ(history.Find(3), nullptr);
}

// the slot carries its own sequence, so a counter that has come all the way
// round reports a miss rather than handing back a stale snapshot under a new
// number -- which would be a silently corrupt baseline.
TEST(SnapshotHistory, EvictedSlotsReportAMissNotStaleData) {
  SnapshotHistory<int, 4> history;
  history.Store(0, 1000);
  EXPECT_NE(history.Find(0), nullptr);

  history.Store(4, 4000);  // same slot as 0
  EXPECT_EQ(history.Find(0), nullptr);
  ASSERT_NE(history.Find(4), nullptr);
  EXPECT_EQ(*history.Find(4), 4000);
}

TEST(SnapshotHistory, NoBaselineUntilSomethingIsAcknowledged) {
  SnapshotHistory<int, 8> history;
  history.Store(1, 100);
  EXPECT_FALSE(history.has_acknowledged());
  EXPECT_EQ(history.AcknowledgedSnapshot(), nullptr);

  history.Acknowledge(1);
  EXPECT_TRUE(history.has_acknowledged());
  ASSERT_NE(history.AcknowledgedSnapshot(), nullptr);
  EXPECT_EQ(*history.AcknowledgedSnapshot(), 100);
}

// acks race on an unreliable link. An older one must not drag the baseline
// backwards.
TEST(SnapshotHistory, OutOfOrderAcksDoNotMoveTheMarkBack) {
  SnapshotHistory<int, 16> history;
  for (uint16_t i = 1; i <= 5; ++i) {
    history.Store(i, i * 10);
  }
  history.Acknowledge(5);
  history.Acknowledge(3);  // arrives late
  EXPECT_EQ(history.acknowledged(), 5);
  EXPECT_EQ(*history.AcknowledgedSnapshot(), 50);
}

TEST(SnapshotHistory, AcksOrderCorrectlyAcrossWrap) {
  SnapshotHistory<int, 16> history;
  history.Acknowledge(65535);
  history.Acknowledge(2);
  EXPECT_EQ(history.acknowledged(), 2);

  SnapshotHistory<int, 16> other;
  other.Acknowledge(2);
  other.Acknowledge(65535);  // older, arriving late
  EXPECT_EQ(other.acknowledged(), 2);
}

// a peer that has been quiet longer than the window must produce nullptr, so
// the sender falls back to a full snapshot instead of deltaing against
// something the peer does not have.
TEST(SnapshotHistory, StaleBaselineAgesOutAndAsksForAFullSnapshot) {
  SnapshotHistory<int, 8> history;
  history.Store(1, 100);
  history.Acknowledge(1);
  ASSERT_NE(history.AcknowledgedSnapshot(), nullptr);

  for (uint16_t i = 2; i <= 20; ++i) {
    history.Store(i, i * 10);
  }
  EXPECT_TRUE(history.has_acknowledged());
  EXPECT_EQ(history.AcknowledgedSnapshot(), nullptr) << "should ask for a full snapshot";
}

TEST(SnapshotHistory, ResetClearsEverything) {
  SnapshotHistory<int, 8> history;
  history.Store(1, 100);
  history.Acknowledge(1);
  history.Reset();
  EXPECT_FALSE(history.has_acknowledged());
  EXPECT_EQ(history.Find(1), nullptr);
  EXPECT_EQ(history.AcknowledgedSnapshot(), nullptr);
}

// ---------------------------------------------------------------------------
// Field-level delta
// ---------------------------------------------------------------------------

TEST(Delta, UnchangedStateCostsOneBitPerField) {
  PlayerState state;
  state.ammo = 137;
  state.health = 80;
  state.yaw = 1.5f;
  state.pitch = -0.25f;
  state.weapon = 3;
  state.firing = false;
  state = Quantized(state);

  auto buffer = MakeBuffer();
  const size_t bits = WriteState(*buffer, state, state);
  // five delta fields at one flag each, plus the always-sent bool
  EXPECT_EQ(bits, 6u);
  EXPECT_EQ(buffer->size(), 1u);

  EXPECT_EQ(ReadState(*buffer, state), state);
}

TEST(Delta, OnlyTheChangedFieldIsSent) {
  PlayerState was;
  was.ammo = 137;
  was.health = 80;
  was.yaw = 1.5f;
  was = Quantized(was);

  PlayerState now = was;
  now.ammo = 136;

  auto buffer = MakeBuffer();
  const size_t bits = WriteState(*buffer, now, was);
  EXPECT_EQ(bits, 6u + 8u);  // the flags, plus the 8-bit ammo field

  EXPECT_EQ(ReadState(*buffer, was), now);
}

TEST(Delta, FullyChangedStateRoundTrips) {
  PlayerState was;
  PlayerState now;
  now.ammo = 200;
  now.health = -50;
  now.yaw = 3.0f;
  now.pitch = -3.0f;
  now.weapon = 15;
  now.firing = true;
  now = Quantized(now);

  auto buffer = MakeBuffer();
  WriteState(*buffer, now, was);
  EXPECT_EQ(ReadState(*buffer, was), now);
}

TEST(Delta, CountersReportWhatWasSent) {
  PlayerState was;
  was.ammo = 50;
  PlayerState now = was;
  now.health = 42;

  auto buffer = MakeBuffer();
  {
    BitWriter bits(*buffer);
    DeltaWriter delta(bits);
    delta.WriteUIntRanged(now.ammo, was.ammo, kAmmoMin, kAmmoMax);
    delta.WriteIntRanged(now.health, was.health, kHealthMin, kHealthMax);
    EXPECT_EQ(delta.fields_written(), 2u);
    EXPECT_EQ(delta.fields_changed(), 1u);
  }
}

// the core correctness rule for lossy fields: "unchanged" must mean the
// *quantised* value is unchanged, because a quantised code is what the
// receiver holds. A drift smaller than one step must not cost a field.
TEST(Delta, DriftInsideOneQuantisationStepIsNotAChange) {
  const float step = (kAngleMax - kAngleMin) /
                     static_cast<float>((1u << kAngleBits) - 1u);
  PlayerState was;
  was.yaw = 1.0f;
  was = Quantized(was);

  PlayerState now = was;
  now.yaw = was.yaw + step * 0.1f;  // far below one step

  auto buffer = MakeBuffer();
  EXPECT_EQ(WriteState(*buffer, now, was), 6u) << "should have sent nothing";
  EXPECT_FLOAT_EQ(ReadState(*buffer, was).yaw, was.yaw);
}

TEST(Delta, MovementLargerThanAStepIsAChange) {
  const float step = (kAngleMax - kAngleMin) /
                     static_cast<float>((1u << kAngleBits) - 1u);
  PlayerState was;
  was.yaw = 1.0f;
  was = Quantized(was);

  PlayerState now = was;
  now.yaw = was.yaw + step * 3.0f;

  auto buffer = MakeBuffer();
  EXPECT_EQ(WriteState(*buffer, now, was), 6u + kAngleBits);
  EXPECT_NEAR(ReadState(*buffer, was).yaw, now.yaw, step);
}

// a field held still for a long run must not creep. Each tick's baseline is
// the previous tick's decoded value, so any bias would compound.
TEST(Delta, HoldingAValueStillDoesNotDrift) {
  PlayerState state;
  state.yaw = 1.2345f;
  state.pitch = -2.7f;
  state = Quantized(state);

  PlayerState was = state;
  for (int tick = 0; tick < 10000; ++tick) {
    auto buffer = MakeBuffer();
    ASSERT_EQ(WriteState(*buffer, state, was), 6u) << "tick " << tick;
    const PlayerState now = ReadState(*buffer, was);
    ASSERT_FLOAT_EQ(now.yaw, state.yaw) << "tick " << tick;
    ASSERT_FLOAT_EQ(now.pitch, state.pitch) << "tick " << tick;
    was = now;
  }
}

TEST(Delta, OutOfRangeBaselineCannotDesyncTheTwoEnds) {
  PlayerState was;
  was.ammo = 999999;    // above kAmmoMax
  was.health = -99999;  // below kHealthMin
  was.yaw = 100.0f;     // way outside the angle range

  PlayerState now = was;  // "unchanged", but nothing is representable

  auto buffer = MakeBuffer();
  WriteState(*buffer, now, was);
  const PlayerState read = ReadState(*buffer, was);
  EXPECT_EQ(read.ammo, kAmmoMax);
  EXPECT_EQ(read.health, kHealthMin);
  EXPECT_NEAR(read.yaw, kAngleMax, 0.01f);
}

// ---------------------------------------------------------------------------
// End to end over a lossy link
// ---------------------------------------------------------------------------

// the scenario the extension exists for, and the one where a delta codec
// actually breaks: packets are dropped, acks come back late, and the sender
// has to keep choosing a baseline the receiver genuinely holds. If it ever
// picks wrong, the receiver decodes a state that is confidently incorrect
// rather than obviously broken -- so the test compares every tick.
TEST(Delta, SurvivesALossyLink) {
  std::mt19937 rng(20260801u);
  std::uniform_int_distribution<int> drop(0, 99);
  std::uniform_int_distribution<int> ammo_step(-3, 3);

  SnapshotHistory<PlayerState, 64> sender_history;
  PlayerState truth;
  truth.ammo = 100;
  truth.health = 50;
  truth = Quantized(truth);

  PlayerState receiver_state;
  bool receiver_has_state = false;
  uint16_t receiver_latest = 0;

  size_t delta_bits = 0;
  size_t delta_packets = 0;
  int delivered = 0;

  for (uint16_t sequence = 1; sequence < 3000; ++sequence) {
    // the game moves on
    const int64_t next_ammo =
        static_cast<int64_t>(truth.ammo) + ammo_step(rng);
    truth.ammo = static_cast<uint64_t>(
        next_ammo < 0 ? 0 : (next_ammo > 200 ? 200 : next_ammo));
    truth.yaw += 0.01f;
    if (truth.yaw > kAngleMax) {
      truth.yaw = kAngleMin;
    }
    truth.firing = (sequence % 7) == 0;
    truth = Quantized(truth);

    sender_history.Store(sequence, truth);

    // encode against whatever the peer has actually confirmed
    const PlayerState* baseline = sender_history.AcknowledgedSnapshot();
    const uint16_t baseline_sequence = sender_history.acknowledged();
    const PlayerState empty;
    const PlayerState& against = baseline != nullptr ? *baseline : empty;

    auto buffer = MakeBuffer();
    const size_t bits = WriteState(*buffer, truth, against);
    if (baseline != nullptr) {
      delta_bits += bits;
      ++delta_packets;
    }

    // 20% of packets never arrive
    if (drop(rng) < 20) {
      continue;
    }

    // the receiver decodes against the same baseline the sender named
    PlayerState receiver_baseline;
    if (baseline != nullptr) {
      ASSERT_TRUE(receiver_has_state)
          << "sender deltaed against seq " << baseline_sequence
          << " but the receiver has nothing";
      receiver_baseline = receiver_state;
      ASSERT_EQ(baseline_sequence, receiver_latest)
          << "sender and receiver disagree about the baseline at seq "
          << sequence;
    }

    const PlayerState decoded = ReadState(*buffer, receiver_baseline);
    ASSERT_EQ(decoded, truth) << "diverged at sequence " << sequence;

    receiver_state = decoded;
    receiver_has_state = true;
    receiver_latest = sequence;
    ++delivered;

    // the ack gets back to the sender (this direction is lossless here; the
    // interesting losses are on the snapshot path)
    sender_history.Acknowledge(sequence);
  }

  EXPECT_GT(delivered, 2000);
  ASSERT_GT(delta_packets, 0u);

  // the point of all this. Compare against what the same state costs with
  // every field sent -- a measured number, not a guessed threshold, so the
  // assertion still means something if the test's field list changes.
  auto full_buffer = MakeBuffer();
  PlayerState nothing_matches;
  nothing_matches.ammo = truth.ammo + 1;
  nothing_matches.health = truth.health + 1;
  nothing_matches.yaw = truth.yaw + 1.0f;
  nothing_matches.pitch = truth.pitch + 1.0f;
  nothing_matches.weapon = truth.weapon + 1;
  const size_t full_state_bits =
      WriteState(*full_buffer, truth, nothing_matches);

  const double average_delta =
      static_cast<double>(delta_bits) / static_cast<double>(delta_packets);
  EXPECT_LT(average_delta, static_cast<double>(full_state_bits) * 0.6)
      << "average delta " << average_delta << " bits against a full state of "
      << full_state_bits;
}

// same link, but the receiver acknowledges only rarely, so the sender keeps
// deltaing against an old baseline and eventually has to notice it has aged
// out of the window.
TEST(Delta, FallsBackToAFullSnapshotWhenTheBaselineAgesOut) {
  SnapshotHistory<PlayerState, 8> history;
  PlayerState truth;
  truth.ammo = 10;

  history.Store(1, truth);
  history.Acknowledge(1);
  ASSERT_NE(history.AcknowledgedSnapshot(), nullptr);

  int full_snapshots = 0;
  for (uint16_t sequence = 2; sequence < 40; ++sequence) {
    truth.ammo = (truth.ammo + 1) % 200;
    history.Store(sequence, truth);
    if (history.AcknowledgedSnapshot() == nullptr) {
      ++full_snapshots;
    }
  }
  EXPECT_GT(full_snapshots, 0)
      << "an aged-out baseline must force a full snapshot";
}

TEST(Delta, TruncatedDeltaIsReportedRatherThanDecodedAsGarbage) {
  PlayerState was;
  PlayerState now;
  now.ammo = 200;
  now.health = 100;
  now.yaw = 3.0f;

  auto buffer = MakeBuffer();
  WriteState(*buffer, now, was);
  buffer->SetReadLimit(1);  // the packet arrives clipped

  BitReader bits(*buffer);
  DeltaReader delta(bits);
  delta.ReadUIntRanged(was.ammo, kAmmoMin, kAmmoMax);
  delta.ReadIntRanged(was.health, kHealthMin, kHealthMax);
  delta.ReadFloatRanged(was.yaw, kAngleMin, kAngleMax, kAngleBits);
  EXPECT_FALSE(bits.ok());
}
