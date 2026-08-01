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

#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>

#include <string>
#include <random>
#include <vector>

#include "znet/ext/entt/entt.h"

using znet::Buffer;
using znet::Endianness;
using znet::ext::EnttInputArchive;
using znet::ext::EnttOutputArchive;

namespace game {

// a component with no default serializer, to exercise the customization point.
struct Position {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  bool operator==(const Position& other) const {
    return x == other.x && y == other.y && z == other.z;
  }
};

// found by argument-dependent lookup: these live beside the component, not
// beside the archive.
void SerializeComponent(Buffer& buffer, const Position& value) {
  buffer.WriteFloat(value.x);
  buffer.WriteFloat(value.y);
  buffer.WriteFloat(value.z);
}

void DeserializeComponent(Buffer& buffer, Position& value) {
  value.x = buffer.ReadFloat();
  value.y = buffer.ReadFloat();
  value.z = buffer.ReadFloat();
}

// an empty component. EnTT stores no payload for these, so the archive is
// never called with one, and the wire format is just the entity list.
struct Invulnerable {};

enum class Team : uint8_t { kRed, kBlue, kGreen };

}  // namespace game

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

struct World {
  entt::registry registry;
  std::vector<entt::entity> entities;
};

/** @brief Builds a registry with @p count entities carrying assorted components. */
World MakeWorld(int count) {
  World world;
  for (int i = 0; i < count; ++i) {
    const entt::entity entity = world.registry.create();
    world.entities.push_back(entity);
    world.registry.emplace<game::Position>(
        entity, game::Position{static_cast<float>(i), static_cast<float>(i * 2),
                               0.5f});
    world.registry.emplace<int>(entity, i * 10);
    if (i % 2 == 0) {
      world.registry.emplace<game::Team>(entity, game::Team::kBlue);
    }
    if (i % 3 == 0) {
      world.registry.emplace<game::Invulnerable>(entity);
    }
  }
  return world;
}

void WriteSnapshot(Buffer& buffer, const entt::registry& registry) {
  EnttOutputArchive archive(buffer);
  entt::snapshot{registry}
      .get<entt::entity>(archive)
      .get<game::Position>(archive)
      .get<int>(archive)
      .get<game::Team>(archive)
      .get<game::Invulnerable>(archive);
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trip through snapshot_loader
// ---------------------------------------------------------------------------

TEST(EnttArchive, RegistryRoundTrips) {
  const World source = MakeWorld(25);

  auto buffer = MakeBuffer();
  WriteSnapshot(*buffer, source.registry);
  ASSERT_GT(buffer->size(), 0u);

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}
      .get<entt::entity>(archive)
      .get<game::Position>(archive)
      .get<int>(archive)
      .get<game::Team>(archive)
      .get<game::Invulnerable>(archive);

  EXPECT_TRUE(archive.ok());

  for (const entt::entity entity : source.entities) {
    ASSERT_TRUE(target.valid(entity));
    EXPECT_EQ(target.get<game::Position>(entity),
              source.registry.get<game::Position>(entity));
    EXPECT_EQ(target.get<int>(entity), source.registry.get<int>(entity));
    EXPECT_EQ(target.all_of<game::Team>(entity),
              source.registry.all_of<game::Team>(entity));
    EXPECT_EQ(target.all_of<game::Invulnerable>(entity),
              source.registry.all_of<game::Invulnerable>(entity));
  }
}

TEST(EnttArchive, EmptyRegistryRoundTrips) {
  const entt::registry source;

  auto buffer = MakeBuffer();
  WriteSnapshot(*buffer, source);

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}
      .get<entt::entity>(archive)
      .get<game::Position>(archive)
      .get<int>(archive)
      .get<game::Team>(archive)
      .get<game::Invulnerable>(archive);

  EXPECT_TRUE(archive.ok());
  EXPECT_EQ(target.storage<entt::entity>().size(), 0u);
}

// components with no payload cost nothing but the entity list, and must not
// invoke the archive's component path at all.
TEST(EnttArchive, TagComponentsSurvive) {
  entt::registry source;
  const entt::entity a = source.create();
  const entt::entity b = source.create();
  source.emplace<game::Invulnerable>(a);

  auto buffer = MakeBuffer();
  {
    EnttOutputArchive archive(*buffer);
    entt::snapshot{source}
        .get<entt::entity>(archive)
        .get<game::Invulnerable>(archive);
  }

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}
      .get<entt::entity>(archive)
      .get<game::Invulnerable>(archive);

  EXPECT_TRUE(target.all_of<game::Invulnerable>(a));
  EXPECT_FALSE(target.all_of<game::Invulnerable>(b));
}

// a registry with holes in its entity list: EnTT marks the gaps with the null
// entity, which is why identifiers are fixed width rather than varints.
TEST(EnttArchive, DestroyedEntitiesLeaveTheListIntact) {
  entt::registry source;
  std::vector<entt::entity> entities;
  for (int i = 0; i < 10; ++i) {
    const entt::entity entity = source.create();
    entities.push_back(entity);
    source.emplace<int>(entity, i);
  }
  source.destroy(entities[3]);
  source.destroy(entities[7]);

  auto buffer = MakeBuffer();
  {
    EnttOutputArchive archive(*buffer);
    entt::snapshot{source}.get<entt::entity>(archive).get<int>(archive);
  }

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}.get<entt::entity>(archive).get<int>(archive);
  EXPECT_TRUE(archive.ok());

  for (int i = 0; i < 10; ++i) {
    if (i == 3 || i == 7) {
      continue;
    }
    ASSERT_TRUE(target.valid(entities[static_cast<size_t>(i)]));
    EXPECT_EQ(target.get<int>(entities[static_cast<size_t>(i)]), i);
  }
}

// ---------------------------------------------------------------------------
// Entity remapping, which is the reason to build on EnTT's loaders
// ---------------------------------------------------------------------------

// a client's registry already holds its own entities, so a server identifier
// almost certainly collides with something local. continuous_loader maps
// server ids onto locally allocated ones and keeps the mapping across updates.
// reimplementing that is where an ECS replication layer usually goes wrong,
// which is why this extension only supplies the archive.
TEST(EnttArchive, ContinuousLoaderRemapsOntoABusyRegistry) {
  const World server = MakeWorld(10);

  entt::registry client;
  // the client is already using identifiers of its own
  for (int i = 0; i < 40; ++i) {
    const entt::entity local = client.create();
    client.emplace<int>(local, -1);
  }

  auto buffer = MakeBuffer();
  WriteSnapshot(*buffer, server.registry);

  entt::continuous_loader loader{client};
  EnttInputArchive archive(*buffer);
  loader.get<entt::entity>(archive)
      .get<game::Position>(archive)
      .get<int>(archive)
      .get<game::Team>(archive)
      .get<game::Invulnerable>(archive);

  EXPECT_TRUE(archive.ok());

  for (const entt::entity remote : server.entities) {
    ASSERT_TRUE(loader.contains(remote));
    const entt::entity local = loader.map(remote);
    ASSERT_TRUE(client.valid(local));
    EXPECT_EQ(client.get<game::Position>(local),
              server.registry.get<game::Position>(remote));
    EXPECT_EQ(client.get<int>(local), server.registry.get<int>(remote));
  }
}

// two updates in a row must land on the same local entities rather than
// creating a second copy of the world.
TEST(EnttArchive, ContinuousLoaderIsStableAcrossUpdates) {
  World server = MakeWorld(8);
  entt::registry client;
  entt::continuous_loader loader{client};

  std::vector<entt::entity> first_pass;
  for (int update = 0; update < 3; ++update) {
    for (const entt::entity entity : server.entities) {
      server.registry.get<game::Position>(entity).x += 1.0f;
    }

    auto buffer = MakeBuffer();
    WriteSnapshot(*buffer, server.registry);

    EnttInputArchive archive(*buffer);
    loader.get<entt::entity>(archive)
        .get<game::Position>(archive)
        .get<int>(archive)
        .get<game::Team>(archive)
        .get<game::Invulnerable>(archive);
    ASSERT_TRUE(archive.ok());

    std::vector<entt::entity> mapped;
    for (const entt::entity remote : server.entities) {
      mapped.push_back(loader.map(remote));
    }
    if (update == 0) {
      first_pass = mapped;
    } else {
      EXPECT_EQ(mapped, first_pass) << "update " << update << " remapped";
    }

    for (const entt::entity remote : server.entities) {
      EXPECT_EQ(client.get<game::Position>(loader.map(remote)),
                server.registry.get<game::Position>(remote));
    }
  }
}

// ---------------------------------------------------------------------------
// Hostile and truncated input
// ---------------------------------------------------------------------------

// the count in a snapshot is attacker-controlled and EnTT uses it to reserve
// storage and to bound its own loop. Unclamped, this packet asks for four
// billion entities from nine bytes.
TEST(EnttArchive, AbsurdCountIsClampedRatherThanHonoured) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<uint32_t>(4000000000u));  // claimed length
  buffer->WriteVarInt(static_cast<uint32_t>(0u));           // free list
  buffer->WriteInt<uint32_t>(1u);                           // one real entity

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}.get<entt::entity>(archive);

  EXPECT_FALSE(archive.ok());
  EXPECT_TRUE(archive.clamped());
  // and it returned promptly rather than looping four billion times
  EXPECT_LT(target.storage<entt::entity>().size(), 100u);
}

// a free list cannot be longer than the storage holding it. EnTT asserts on
// that rather than checking it, so a packet claiming otherwise is an assertion
// failure in a debug build and a corrupt storage in a release one.
TEST(EnttArchive, FreeListLongerThanTheEntityListIsClamped) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<uint32_t>(2));    // two entities
  buffer->WriteVarInt(static_cast<uint32_t>(999));  // ... and 999 of them free
  buffer->WriteInt<uint32_t>(0u);
  buffer->WriteInt<uint32_t>(1u);

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}.get<entt::entity>(archive);

  EXPECT_TRUE(archive.clamped());
}

// truncation must not hand the loader the same identifier twice. It yields the
// null entity instead, which EnTT already understands.
TEST(EnttArchive, TruncationDoesNotRepeatAnIdentifier) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<uint32_t>(6));  // claims six entities
  buffer->WriteVarInt(static_cast<uint32_t>(0));
  buffer->WriteInt<uint32_t>(1u);  // ... and carries one
  buffer->WriteInt<uint32_t>(2u);

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}.get<entt::entity>(archive);

  EXPECT_FALSE(archive.ok());
}

TEST(EnttArchive, TruncatedSnapshotIsReported) {
  const World source = MakeWorld(20);

  auto buffer = MakeBuffer();
  WriteSnapshot(*buffer, source.registry);
  buffer->SetReadLimit(buffer->size() / 4);  // arrives clipped

  entt::registry target;
  EnttInputArchive archive(*buffer);
  entt::snapshot_loader{target}
      .get<entt::entity>(archive)
      .get<game::Position>(archive)
      .get<int>(archive);

  EXPECT_FALSE(archive.ok());
}

// whatever the bytes are, loading must terminate and must not leave the
// process worse off. The registry contents are meaningless here; not hanging
// and not allocating wildly is the whole assertion.
TEST(EnttArchive, ArbitraryBytesDoNotHangTheLoader) {
  std::mt19937 rng(4242u);
  for (int trial = 0; trial < 20000; ++trial) {
    auto buffer = MakeBuffer();
    const unsigned length = 1 + (rng() % 64);
    for (unsigned i = 0; i < length; ++i) {
      buffer->WriteInt<uint8_t>(static_cast<uint8_t>(rng()));
    }

    entt::registry target;
    EnttInputArchive archive(*buffer);
    entt::snapshot_loader{target}
        .get<entt::entity>(archive)
        .get<game::Position>(archive)
        .get<int>(archive);

    ASSERT_LT(target.storage<entt::entity>().size(), 1000u)
        << "trial " << trial;
  }
  SUCCEED();
}

// ---------------------------------------------------------------------------
// The component customization point
// ---------------------------------------------------------------------------

TEST(EnttComponent, BuiltInDefaultsRoundTrip) {
  auto buffer = MakeBuffer();
  {
    EnttOutputArchive archive(*buffer);
    archive(int32_t{-42});
    archive(3.5f);
    archive(std::string("hello"));
    archive(game::Team::kGreen);
    archive(true);
  }

  EnttInputArchive archive(*buffer);
  int32_t i = 0;
  float f = 0.0f;
  std::string s;
  game::Team team = game::Team::kRed;
  bool b = false;
  archive(i);
  archive(f);
  archive(s);
  archive(team);
  archive(b);

  EXPECT_EQ(i, -42);
  EXPECT_FLOAT_EQ(f, 3.5f);
  EXPECT_EQ(s, "hello");
  EXPECT_EQ(team, game::Team::kGreen);
  EXPECT_TRUE(b);
}

// bool goes through an integer rather than a raw byte, so a byte that is
// neither 0 nor 1 still yields a usable bool.
TEST(EnttComponent, BoolToleratesNonCanonicalBytes) {
  auto buffer = MakeBuffer();
  buffer->WriteInt<uint8_t>(0xFFu);

  EnttInputArchive archive(*buffer);
  bool value = false;
  archive(value);
  EXPECT_TRUE(value);
  EXPECT_NE(value, !value);
}

TEST(EnttComponent, AdlOverloadIsFound) {
  auto buffer = MakeBuffer();
  const game::Position written{1.5f, -2.5f, 3.0f};
  {
    EnttOutputArchive archive(*buffer);
    archive(written);
  }
  EXPECT_EQ(buffer->size(), sizeof(float) * 3);

  EnttInputArchive archive(*buffer);
  game::Position read;
  archive(read);
  EXPECT_EQ(read, written);
}
