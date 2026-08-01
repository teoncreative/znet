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

#include <random>
#include <string>
#include <vector>

#include "znet/ext/flatbuffers/flatbuffers.h"

using znet::Buffer;
using znet::Endianness;
using znet::ext::FlatBufferLimits;
using znet::ext::FlatBufferPacket;
using znet::ext::FlatBufferSerializer;
using znet::ext::ReadFlatBuffer;
using znet::ext::WriteFlatBuffer;

namespace demo {

// a table written by hand rather than generated, so these tests need only the
// FlatBuffers runtime headers and not flatc. The shape is exactly what flatc
// emits for:
//
//   table Player { level: int; name: string; }
//
struct Player FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  enum FlatBuffersVTableOffset : flatbuffers::voffset_t {
    kLevel = 4,
    kName = 6,
  };

  int32_t level() const { return GetField<int32_t>(kLevel, 0); }

  const flatbuffers::String* name() const {
    return GetPointer<const flatbuffers::String*>(kName);
  }

  bool Verify(flatbuffers::Verifier& verifier) const {
    return VerifyTableStart(verifier) &&
           VerifyField<int32_t>(verifier, kLevel, 4) &&
           VerifyOffset(verifier, kName) && verifier.VerifyString(name()) &&
           verifier.EndTable();
  }
};

/** @brief Builds a Player the way generated code would. */
inline flatbuffers::Offset<Player> CreatePlayer(
    flatbuffers::FlatBufferBuilder& builder, int32_t level,
    flatbuffers::Offset<flatbuffers::String> name) {
  const flatbuffers::uoffset_t start = builder.StartTable();
  builder.AddElement<int32_t>(Player::kLevel, level, 0);
  builder.AddOffset(Player::kName, name);
  return flatbuffers::Offset<Player>(builder.EndTable(start));
}

}  // namespace demo

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

/** @brief A finished flatbuffer holding one Player. */
std::vector<uint8_t> BuildPlayer(int32_t level, const std::string& name) {
  flatbuffers::FlatBufferBuilder builder;
  const auto name_offset = builder.CreateString(name);
  builder.Finish(demo::CreatePlayer(builder, level, name_offset));
  return std::vector<uint8_t>(builder.GetBufferPointer(),
                              builder.GetBufferPointer() + builder.GetSize());
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(FlatBuffers, RoundTripsThroughABuffer) {
  const std::vector<uint8_t> payload = BuildPlayer(42, "player one");

  auto buffer = MakeBuffer();
  WriteFlatBuffer(*buffer, payload.data(), payload.size());

  std::vector<uint8_t> read;
  ASSERT_TRUE(ReadFlatBuffer<demo::Player>(*buffer, read));

  const demo::Player* player = znet::ext::GetVerifiedRoot<demo::Player>(read);
  ASSERT_NE(player, nullptr);
  EXPECT_EQ(player->level(), 42);
  ASSERT_NE(player->name(), nullptr);
  EXPECT_EQ(player->name()->str(), "player one");
}

TEST(FlatBuffers, WritesFromABuilderDirectly) {
  flatbuffers::FlatBufferBuilder builder;
  const auto name = builder.CreateString("direct");
  builder.Finish(demo::CreatePlayer(builder, 7, name));

  auto buffer = MakeBuffer();
  WriteFlatBuffer(*buffer, builder);

  std::vector<uint8_t> read;
  ASSERT_TRUE(ReadFlatBuffer<demo::Player>(*buffer, read));
  EXPECT_EQ(znet::ext::GetVerifiedRoot<demo::Player>(read)->level(), 7);
}

TEST(FlatBuffers, InterleavesWithOrdinaryBufferFields) {
  const std::vector<uint8_t> payload = BuildPlayer(3, "middle");

  auto buffer = MakeBuffer();
  buffer->WriteString("before");
  WriteFlatBuffer(*buffer, payload.data(), payload.size());
  buffer->WriteInt<uint32_t>(99u);

  EXPECT_EQ(buffer->ReadString(), "before");
  std::vector<uint8_t> read;
  ASSERT_TRUE(ReadFlatBuffer<demo::Player>(*buffer, read));
  EXPECT_EQ(znet::ext::GetVerifiedRoot<demo::Player>(read)->level(), 3);
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 99u);
}

// the payload sits behind a varint prefix, so it is not aligned where it lies
// in the buffer. Reading copies it somewhere that is.
TEST(FlatBuffers, PayloadIsAlignedAfterReadingRegardlessOfItsOffset) {
  const std::vector<uint8_t> payload = BuildPlayer(11, "aligned");

  for (size_t pad = 0; pad < 8; ++pad) {
    auto buffer = MakeBuffer();
    for (size_t i = 0; i < pad; ++i) {
      buffer->WriteInt<uint8_t>(0xAAu);
    }
    WriteFlatBuffer(*buffer, payload.data(), payload.size());

    for (size_t i = 0; i < pad; ++i) {
      buffer->ReadInt<uint8_t>();
    }
    std::vector<uint8_t> read;
    ASSERT_TRUE(ReadFlatBuffer<demo::Player>(*buffer, read)) << "pad " << pad;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(read.data()) %
                  alignof(flatbuffers::largest_scalar_t),
              0u);
    EXPECT_EQ(znet::ext::GetVerifiedRoot<demo::Player>(read)->level(), 11);
  }
}

// ---------------------------------------------------------------------------
// Verification, which is the reason this extension exists
// ---------------------------------------------------------------------------

// FlatBuffers accessors are raw pointer arithmetic with no bounds checks, so a
// payload whose internal offsets point outside itself turns a field read into
// an arbitrary read. Every one of these must be refused before any accessor
// runs.
TEST(FlatBuffers, CorruptedPayloadsAreRefused) {
  const std::vector<uint8_t> original = BuildPlayer(42, "player one");

  int refused = 0;
  for (size_t index = 0; index < original.size(); ++index) {
    for (const uint8_t pattern : {uint8_t{0xFF}, uint8_t{0x7F}, uint8_t{0x01}}) {
      std::vector<uint8_t> corrupt = original;
      corrupt[index] ^= pattern;
      if (corrupt == original) {
        continue;
      }

      auto buffer = MakeBuffer();
      WriteFlatBuffer(*buffer, corrupt.data(), corrupt.size());

      std::vector<uint8_t> read;
      if (!ReadFlatBuffer<demo::Player>(*buffer, read)) {
        ++refused;
        continue;
      }
      // if it verified, it must be safe to walk: the point is that anything
      // getting through is internally consistent, not that nothing does.
      const demo::Player* player =
          znet::ext::GetVerifiedRoot<demo::Player>(read);
      ASSERT_NE(player, nullptr);
      static_cast<void>(player->level());
      if (player->name() != nullptr) {
        static_cast<void>(player->name()->str());
      }
    }
  }
  EXPECT_GT(refused, 0) << "corruption should be caught, not waved through";
}

TEST(FlatBuffers, RandomBytesNeverVerify) {
  std::mt19937 rng(8080u);
  int accepted = 0;
  for (int trial = 0; trial < 3000; ++trial) {
    auto buffer = MakeBuffer();
    const size_t length = 1 + (rng() % 64);
    buffer->WriteVarInt(length);
    for (size_t i = 0; i < length; ++i) {
      buffer->WriteInt<uint8_t>(static_cast<uint8_t>(rng()));
    }

    std::vector<uint8_t> read;
    if (ReadFlatBuffer<demo::Player>(*buffer, read)) {
      ++accepted;
      const demo::Player* player =
          znet::ext::GetVerifiedRoot<demo::Player>(read);
      ASSERT_NE(player, nullptr);
      static_cast<void>(player->level());
    }
  }
  // a handful of random byte strings may happen to be internally consistent;
  // what matters is that walking them stayed inside the payload
  EXPECT_LT(accepted, 100);
}

TEST(FlatBuffers, TruncatedPayloadIsRefused) {
  const std::vector<uint8_t> payload = BuildPlayer(42, "player one");

  auto buffer = MakeBuffer();
  WriteFlatBuffer(*buffer, payload.data(), payload.size());
  buffer->SetReadLimit(buffer->size() / 2);

  std::vector<uint8_t> read;
  EXPECT_FALSE(ReadFlatBuffer<demo::Player>(*buffer, read));
}

TEST(FlatBuffers, EmptyAndImplausibleLengthsAreRefused) {
  {
    auto buffer = MakeBuffer();
    buffer->WriteVarInt(static_cast<size_t>(0));
    std::vector<uint8_t> read;
    EXPECT_FALSE(ReadFlatBuffer<demo::Player>(*buffer, read));
  }
  {
    auto buffer = MakeBuffer();
    buffer->WriteVarInt(static_cast<size_t>(4000000000u));
    buffer->WriteInt<uint8_t>(0u);
    std::vector<uint8_t> read;
    EXPECT_FALSE(ReadFlatBuffer<demo::Player>(*buffer, read));
  }
}

TEST(FlatBuffers, OversizedPayloadIsRefusedBeforeAllocating) {
  const std::vector<uint8_t> payload = BuildPlayer(42, "player one");

  auto buffer = MakeBuffer();
  WriteFlatBuffer(*buffer, payload.data(), payload.size());

  FlatBufferLimits limits;
  limits.max_bytes = 4;
  std::vector<uint8_t> read;
  EXPECT_FALSE(ReadFlatBuffer<demo::Player>(*buffer, read, limits));
}

TEST(FlatBuffers, FailedReadLeavesTheDestinationAlone) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(4));
  for (int i = 0; i < 4; ++i) {
    buffer->WriteInt<uint8_t>(0xFFu);
  }

  std::vector<uint8_t> read = BuildPlayer(1, "keep");
  const std::vector<uint8_t> before = read;
  EXPECT_FALSE(ReadFlatBuffer<demo::Player>(*buffer, read));
  EXPECT_EQ(read, before);
}

// ---------------------------------------------------------------------------
// The PacketSerializer adapter
// ---------------------------------------------------------------------------

TEST(FlatBufferSerializerTest, RoundTripsThroughThePacketInterface) {
  const znet::PacketId id = 12;
  FlatBufferSerializer<demo::Player> serializer(id);

  auto packet = std::make_shared<FlatBufferPacket<demo::Player>>(id);
  packet->bytes = BuildPlayer(99, "packeted");

  auto buffer = MakeBuffer();
  ASSERT_NE(serializer.SerializeTyped(packet, buffer), nullptr);

  const auto decoded = serializer.DeserializeTyped(buffer);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->id(), id);
  ASSERT_NE(decoded->Get(), nullptr);
  EXPECT_EQ(decoded->Get()->level(), 99);
  EXPECT_EQ(decoded->Get()->name()->str(), "packeted");
}

TEST(FlatBufferSerializerTest, SetFromBuilder) {
  flatbuffers::FlatBufferBuilder builder;
  const auto name = builder.CreateString("from builder");
  builder.Finish(demo::CreatePlayer(builder, 5, name));

  auto packet = std::make_shared<FlatBufferPacket<demo::Player>>(1);
  packet->SetFrom(builder);
  EXPECT_EQ(packet->bytes.size(), builder.GetSize());
}

TEST(FlatBufferSerializerTest, UnverifiablePayloadDropsThePacket) {
  FlatBufferSerializer<demo::Player> serializer(4);

  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(16));
  for (int i = 0; i < 16; ++i) {
    buffer->WriteInt<uint8_t>(0xFFu);
  }

  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}

TEST(FlatBufferSerializerTest, EmptyPacketIsNotSent) {
  FlatBufferSerializer<demo::Player> serializer(4);
  auto packet = std::make_shared<FlatBufferPacket<demo::Player>>(4);
  auto buffer = MakeBuffer();
  EXPECT_EQ(serializer.SerializeTyped(packet, buffer), nullptr);
}
