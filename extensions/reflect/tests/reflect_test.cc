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

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "znet/ext/reflect/reflect_all.h"

using znet::Buffer;
using znet::Endianness;
using znet::ext::AutoPacket;
using znet::ext::AutoSerializer;
using znet::ext::FieldCountOf;
using znet::ext::ReadAuto;
using znet::ext::ReflectLimits;
using znet::ext::WriteAuto;

namespace game {

enum class Team : uint8_t { kRed, kBlue };

// a plain aggregate. Nothing is declared about it anywhere.
struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  bool operator==(const Vec3& o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};

struct Player {
  uint32_t id = 0;
  std::string name;
  float health = 0.0f;
  bool alive = false;
  Team team = Team::kRed;
  Vec3 position;
  std::vector<uint32_t> items;
  std::map<std::string, int32_t> stats;
  std::array<uint16_t, 3> slots{};

  bool operator==(const Player& o) const {
    return id == o.id && name == o.name && health == o.health &&
           alive == o.alive && team == o.team && position == o.position &&
           items == o.items && stats == o.stats && slots == o.slots;
  }
};

// not an aggregate: it has a user-declared constructor, so deduction cannot
// touch it and the macro has to.
class Session {
 public:
  Session() = default;
  Session(uint64_t initial_token, std::string initial_label)
      : token(initial_token), label(std::move(initial_label)) {}

  uint64_t token = 0;
  std::string label;

  bool operator==(const Session& o) const {
    return token == o.token && label == o.label;
  }
};

// an aggregate that deliberately keeps a field off the wire, by declaring the
// ones that should travel.
struct Cached {
  uint32_t id = 0;
  std::string name;
  int scratch = 0;  // local only

  bool operator==(const Cached& o) const {
    return id == o.id && name == o.name;
  }
};

}  // namespace game

ZNET_REFLECT(game::Session, token, label)
ZNET_REFLECT(game::Cached, id, name)

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

game::Player SamplePlayer() {
  game::Player p;
  p.id = 4242;
  p.name = "player one";
  p.health = 87.5f;
  p.alive = true;
  p.team = game::Team::kBlue;
  p.position = game::Vec3{1.5f, -2.0f, 0.25f};
  p.items = {1, 2, 3, 5, 8};
  p.stats = {{"kills", 7}, {"deaths", 2}};
  p.slots = {10, 20, 30};
  return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Deduction
// ---------------------------------------------------------------------------

TEST(Reflect, CountsAggregateFields) {
  EXPECT_EQ(FieldCountOf<game::Vec3>(), 3u);
  EXPECT_EQ(FieldCountOf<game::Player>(), 9u);
}

TEST(Reflect, DeclarationWinsOverDeduction) {
  // cached has three members but only two declared, and the declaration is
  // what counts.
  EXPECT_EQ(FieldCountOf<game::Cached>(), 2u);
  EXPECT_EQ(znet::ext::Reflect<game::Cached>::field_count, 2u);
  EXPECT_STREQ(znet::ext::Reflect<game::Cached>::name(), "game::Cached");
}

TEST(Reflect, KnowsWhatItCanSerialize) {
  EXPECT_TRUE(znet::ext::IsSerializable<game::Vec3>::value);
  EXPECT_TRUE(znet::ext::IsSerializable<game::Player>::value);
  EXPECT_TRUE(znet::ext::IsSerializable<game::Session>::value);
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(Reflect, PlainAggregateRoundTripsWithNothingDeclared) {
  const game::Vec3 written{1.5f, -2.5f, 3.0f};

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);
  EXPECT_EQ(buffer->size(), sizeof(float) * 3);

  game::Vec3 read;
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_EQ(read, written);
}

TEST(Reflect, EveryBuiltInCategoryRoundTrips) {
  const game::Player written = SamplePlayer();

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);

  game::Player read;
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_EQ(read, written);
}

TEST(Reflect, DeclaredNonAggregateRoundTrips) {
  const game::Session written{0xDEADBEEFCAFEull, "lobby"};

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);

  game::Session read;
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_EQ(read, written);
}

TEST(Reflect, UndeclaredFieldStaysOffTheWire) {
  game::Cached written;
  written.id = 9;
  written.name = "kept";
  written.scratch = 12345;

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);

  game::Cached read;
  read.scratch = -1;
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_EQ(read.id, 9u);
  EXPECT_EQ(read.name, "kept");
  EXPECT_EQ(read.scratch, -1) << "a field left out of the declaration should "
                                 "not be touched by a read";
}

TEST(Reflect, EmptyContainersRoundTrip) {
  game::Player written;
  written.name.clear();
  written.items.clear();
  written.stats.clear();

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);

  game::Player read = SamplePlayer();
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_TRUE(read.name.empty());
  EXPECT_TRUE(read.items.empty());
  EXPECT_TRUE(read.stats.empty());
}

TEST(Reflect, NestedAggregatesRoundTrip) {
  struct Inner {
    int32_t a;
    int32_t b;
  };
  struct Outer {
    Inner first;
    Inner second;
    uint8_t tag;
  };

  const Outer written{{1, 2}, {3, 4}, 7};
  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);

  Outer read{};
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_EQ(read.first.a, 1);
  EXPECT_EQ(read.first.b, 2);
  EXPECT_EQ(read.second.a, 3);
  EXPECT_EQ(read.second.b, 4);
  EXPECT_EQ(read.tag, 7);
}

TEST(Reflect, VectorOfAggregatesRoundTrips) {
  struct Row {
    uint16_t key;
    float value;
  };
  std::vector<Row> written;
  for (uint16_t i = 0; i < 50; ++i) {
    written.push_back(Row{i, static_cast<float>(i) * 0.5f});
  }

  struct Table {
    std::vector<Row> rows;
  };
  const Table table{written};

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, table);

  Table read;
  ASSERT_TRUE(ReadAuto(*buffer, read));
  ASSERT_EQ(read.rows.size(), written.size());
  for (size_t i = 0; i < written.size(); ++i) {
    EXPECT_EQ(read.rows[i].key, written[i].key);
    EXPECT_FLOAT_EQ(read.rows[i].value, written[i].value);
  }
}

TEST(Reflect, InterleavesWithOrdinaryBufferFields) {
  auto buffer = MakeBuffer();
  buffer->WriteString("header");
  WriteAuto(*buffer, game::Vec3{1.0f, 2.0f, 3.0f});
  buffer->WriteInt<uint32_t>(99u);

  EXPECT_EQ(buffer->ReadString(), "header");
  game::Vec3 read;
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_EQ(read, (game::Vec3{1.0f, 2.0f, 3.0f}));
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 99u);
}

// bool goes through an integer rather than a raw byte, so a byte that is
// neither 0 nor 1 still yields a usable bool.
TEST(Reflect, BoolToleratesNonCanonicalBytes) {
  struct Flag {
    bool value;
  };
  auto buffer = MakeBuffer();
  buffer->WriteInt<uint8_t>(0xFFu);

  Flag read{};
  ASSERT_TRUE(ReadAuto(*buffer, read));
  EXPECT_TRUE(read.value);
  EXPECT_NE(read.value, !read.value);
}

// ---------------------------------------------------------------------------
// Truncated and hostile input
// ---------------------------------------------------------------------------

TEST(Reflect, TruncatedPayloadIsRefused) {
  auto buffer = MakeBuffer();
  WriteAuto(*buffer, SamplePlayer());
  buffer->SetReadLimit(buffer->size() / 3);

  game::Player read;
  EXPECT_FALSE(ReadAuto(*buffer, read));
}

// a container length is attacker-controlled and about to size an allocation.
TEST(Reflect, ImplausibleContainerLengthIsRefused) {
  struct Items {
    std::vector<uint32_t> items;
  };
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(4000000000u));
  buffer->WriteInt<uint32_t>(1u);

  Items read;
  EXPECT_FALSE(ReadAuto(*buffer, read));
  EXPECT_TRUE(read.items.empty());
}

TEST(Reflect, ElementCeilingIsHonoured) {
  struct Items {
    std::vector<uint8_t> items;
  };
  Items written;
  written.items.resize(500, 7);

  auto buffer = MakeBuffer();
  WriteAuto(*buffer, written);

  ReflectLimits limits;
  limits.max_elements = 100;
  Items read;
  EXPECT_FALSE(ReadAuto(*buffer, read, limits));
}

TEST(Reflect, ImplausibleStringLengthIsRefused) {
  struct Named {
    std::string name;
  };
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(4000000000u));
  buffer->WriteInt<uint8_t>(65u);

  Named read;
  EXPECT_FALSE(ReadAuto(*buffer, read));
}

// whatever the bytes are, decoding returns a verdict and stays inside the
// payload rather than allocating wildly or running away.
TEST(Reflect, ArbitraryBytesDecodeSafely) {
  std::mt19937 rng(20260801u);
  for (int trial = 0; trial < 5000; ++trial) {
    auto buffer = MakeBuffer();
    const size_t length = 1 + (rng() % 64);
    for (size_t i = 0; i < length; ++i) {
      buffer->WriteInt<uint8_t>(static_cast<uint8_t>(rng()));
    }

    game::Player read;
    static_cast<void>(ReadAuto(*buffer, read));
    ASSERT_LE(read.items.size(), 64u) << "trial " << trial;
    ASSERT_LE(read.stats.size(), 64u) << "trial " << trial;
    ASSERT_LE(read.name.size(), 64u) << "trial " << trial;
  }
}

// ---------------------------------------------------------------------------
// The packet adapter
// ---------------------------------------------------------------------------

TEST(AutoSerializerTest, RoundTripsThroughThePacketInterface) {
  const znet::PacketId id = 21;
  AutoSerializer<game::Player> serializer(id);

  auto packet = std::make_shared<AutoPacket<game::Player>>(id, SamplePlayer());

  auto buffer = MakeBuffer();
  ASSERT_NE(serializer.SerializeTyped(packet, buffer), nullptr);

  const auto decoded = serializer.DeserializeTyped(buffer);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->id(), id);
  EXPECT_EQ(decoded->body, SamplePlayer());
}

TEST(AutoSerializerTest, MalformedBodyDropsThePacket) {
  AutoSerializer<game::Player> serializer(3);

  auto buffer = MakeBuffer();
  buffer->WriteInt<uint32_t>(1u);  // an id and then nothing else

  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}

TEST(AutoSerializerTest, MakeAutoSerializerBuildsOne) {
  auto serializer = znet::ext::MakeAutoSerializer<game::Vec3>(5);
  ASSERT_NE(serializer, nullptr);

  auto packet = std::make_shared<AutoPacket<game::Vec3>>(
      5, game::Vec3{1.0f, 2.0f, 3.0f});
  auto buffer = MakeBuffer();
  ASSERT_NE(serializer->SerializeTyped(packet, buffer), nullptr);
  const auto decoded = serializer->DeserializeTyped(buffer);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->body, (game::Vec3{1.0f, 2.0f, 3.0f}));
}
