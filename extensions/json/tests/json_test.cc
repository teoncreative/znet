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

#include "znet/ext/json/json.h"

using json = nlohmann::json;
using znet::Buffer;
using znet::Endianness;
using znet::ext::JsonLimits;
using znet::ext::JsonPacket;
using znet::ext::JsonSerializer;
using znet::ext::ReadJson;
using znet::ext::ReadJsonText;
using znet::ext::WriteJson;
using znet::ext::WriteJsonText;

namespace {

std::shared_ptr<Buffer> MakeBuffer() {
  return std::make_shared<Buffer>(Endianness::LittleEndian);
}

json SampleDocument() {
  return json{{"name", "player one"},
              {"level", 42},
              {"health", 87.5},
              {"alive", true},
              {"inventory", {"sword", "rope", "lamp"}},
              {"position", {{"x", 1.5}, {"y", -2.0}, {"z", 0.0}}},
              {"nothing", nullptr}};
}

/** @brief A msgpack payload nesting @p depth arrays, wrapped for the wire. */
std::shared_ptr<Buffer> MakeNestedMsgpack(size_t depth) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(depth + 1);
  for (size_t i = 0; i < depth; ++i) {
    buffer->WriteInt<uint8_t>(0x91u);  // fixarray, one element
  }
  buffer->WriteInt<uint8_t>(0xC0u);  // nil
  return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(Json, MsgpackRoundTrips) {
  const json written = SampleDocument();

  auto buffer = MakeBuffer();
  WriteJson(*buffer, written);

  json read;
  ASSERT_TRUE(ReadJson(*buffer, read));
  EXPECT_EQ(read, written);
}

TEST(Json, TextRoundTrips) {
  const json written = SampleDocument();

  auto buffer = MakeBuffer();
  WriteJsonText(*buffer, written);

  json read;
  ASSERT_TRUE(ReadJsonText(*buffer, read));
  EXPECT_EQ(read, written);
}

TEST(Json, MsgpackIsSmallerThanText) {
  const json document = SampleDocument();

  auto binary = MakeBuffer();
  WriteJson(*binary, document);
  auto text = MakeBuffer();
  WriteJsonText(*text, document);

  EXPECT_LT(binary->size(), text->size());
}

TEST(Json, EmptyAndScalarDocuments) {
  const json documents[] = {json::object(), json::array(), json(nullptr),
                            json(0),        json(""),      json(false)};
  for (const json& written : documents) {
    auto buffer = MakeBuffer();
    WriteJson(*buffer, written);
    json read;
    ASSERT_TRUE(ReadJson(*buffer, read)) << written.dump();
    EXPECT_EQ(read, written);
  }
}

TEST(Json, InterleavesWithOrdinaryBufferFields) {
  auto buffer = MakeBuffer();
  buffer->WriteString("header");
  WriteJson(*buffer, SampleDocument());
  buffer->WriteInt<uint32_t>(7u);

  EXPECT_EQ(buffer->ReadString(), "header");
  json read;
  ASSERT_TRUE(ReadJson(*buffer, read));
  EXPECT_EQ(read, SampleDocument());
  EXPECT_EQ(buffer->ReadInt<uint32_t>(), 7u);
}

TEST(Json, DiscardedValueWritesAsNullRatherThanThrowing) {
  auto buffer = MakeBuffer();
  const json discarded = json::parse("{invalid", nullptr, false);
  ASSERT_TRUE(discarded.is_discarded());

  WriteJson(*buffer, discarded);
  json read;
  ASSERT_TRUE(ReadJson(*buffer, read));
  EXPECT_TRUE(read.is_null());
}

// ---------------------------------------------------------------------------
// Depth: a remote crash if left unbounded
// ---------------------------------------------------------------------------

// nlohmann's MessagePack reader is recursive. A hundred thousand repeated 0x91
// bytes, about a hundred kilobytes on the wire, nests a hundred thousand
// arrays and overflows the stack. Left to from_msgpack alone this test would
// not fail, it would take the process down.
TEST(Json, DeeplyNestedMsgpackIsRejectedNotCrashed) {
  auto buffer = MakeNestedMsgpack(200000);

  json read;
  EXPECT_FALSE(ReadJson(*buffer, read));
}

TEST(Json, NestingAtTheLimitIsAcceptedJustBeyondItIsNot) {
  JsonLimits limits;
  limits.max_depth = 16;

  {
    auto buffer = MakeNestedMsgpack(16);
    json read;
    EXPECT_TRUE(ReadJson(*buffer, read, limits));
  }
  {
    auto buffer = MakeNestedMsgpack(17);
    json read;
    EXPECT_FALSE(ReadJson(*buffer, read, limits));
  }
}

TEST(Json, DeeplyNestedTextIsRejectedToo) {
  JsonLimits limits;
  limits.max_depth = 8;

  auto buffer = MakeBuffer();
  const std::string text = std::string(64, '[') + std::string(64, ']');
  buffer->WriteVarInt(text.size());
  buffer->Write(text.data(), text.size());

  json read;
  EXPECT_FALSE(ReadJsonText(*buffer, read, limits));
}

// depth is counted, not merely bounded by payload length: a wide document is
// fine however large, only a deep one is refused.
TEST(Json, WideDocumentsAreNotMistakenForDeepOnes) {
  json wide = json::array();
  for (int i = 0; i < 20000; ++i) {
    wide.push_back(i);
  }

  auto buffer = MakeBuffer();
  WriteJson(*buffer, wide);

  JsonLimits limits;
  limits.max_depth = 4;
  json read;
  ASSERT_TRUE(ReadJson(*buffer, read, limits));
  EXPECT_EQ(read.size(), 20000u);
}

// ---------------------------------------------------------------------------
// Size, truncation and garbage
// ---------------------------------------------------------------------------

TEST(Json, OversizedPayloadIsRefusedBeforeDecoding) {
  auto buffer = MakeBuffer();
  WriteJson(*buffer, SampleDocument());

  JsonLimits limits;
  limits.max_bytes = 4;
  json read;
  EXPECT_FALSE(ReadJson(*buffer, read, limits));
}

// a length that exceeds what the packet carries must be refused rather than
// used to size an allocation.
TEST(Json, ImplausibleLengthIsRefused) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(4000000000u));
  buffer->WriteInt<uint8_t>(0xC0u);

  json read;
  EXPECT_FALSE(ReadJson(*buffer, read));
}

TEST(Json, TruncatedPayloadIsRefused) {
  auto buffer = MakeBuffer();
  WriteJson(*buffer, SampleDocument());
  buffer->SetReadLimit(buffer->size() / 2);

  json read;
  EXPECT_FALSE(ReadJson(*buffer, read));
}

TEST(Json, FailedReadLeavesTheDestinationAlone) {
  auto buffer = MakeBuffer();
  buffer->WriteVarInt(static_cast<size_t>(3));
  buffer->WriteInt<uint8_t>(0xC1u);  // never-valid msgpack byte
  buffer->WriteInt<uint8_t>(0xC1u);
  buffer->WriteInt<uint8_t>(0xC1u);

  json read = json{{"keep", "me"}};
  EXPECT_FALSE(ReadJson(*buffer, read));
  EXPECT_EQ(read, json({{"keep", "me"}}));
}

// whatever the bytes are, decoding returns a verdict rather than throwing:
// an exception escaping here would take down the session, not just the packet.
TEST(Json, ArbitraryBytesNeverThrow) {
  std::mt19937 rng(31337u);
  for (int trial = 0; trial < 3000; ++trial) {
    auto buffer = MakeBuffer();
    const size_t length = 1 + (rng() % 48);
    buffer->WriteVarInt(length);
    for (size_t i = 0; i < length; ++i) {
      buffer->WriteInt<uint8_t>(static_cast<uint8_t>(rng()));
    }

    json read;
    ASSERT_NO_THROW({
      static_cast<void>(ReadJson(*buffer, read));
    }) << "trial " << trial;
  }
}

TEST(Json, ArbitraryTextNeverThrows) {
  std::mt19937 rng(999u);
  const char alphabet[] = "{}[]\",:0123456789abcnul \\/";
  for (int trial = 0; trial < 3000; ++trial) {
    auto buffer = MakeBuffer();
    const size_t length = 1 + (rng() % 48);
    std::string text;
    for (size_t i = 0; i < length; ++i) {
      text += alphabet[rng() % (sizeof(alphabet) - 1)];
    }
    buffer->WriteVarInt(text.size());
    buffer->Write(text.data(), text.size());

    json read;
    ASSERT_NO_THROW({
      static_cast<void>(ReadJsonText(*buffer, read));
    }) << "trial " << trial;
  }
}

// ---------------------------------------------------------------------------
// The PacketSerializer adapter
// ---------------------------------------------------------------------------

TEST(JsonSerializerTest, RoundTripsThroughThePacketInterface) {
  const znet::PacketId id = 7;
  JsonSerializer serializer(id);

  auto packet = std::make_shared<JsonPacket>(id);
  packet->body = SampleDocument();

  auto buffer = MakeBuffer();
  ASSERT_NE(serializer.SerializeTyped(packet, buffer), nullptr);

  const auto decoded = serializer.DeserializeTyped(buffer);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->id(), id);
  EXPECT_EQ(decoded->body, SampleDocument());
}

TEST(JsonSerializerTest, TextModeRoundTrips) {
  const znet::PacketId id = 9;
  JsonSerializer serializer(id);
  serializer.set_text(true);

  auto packet = std::make_shared<JsonPacket>(id);
  packet->body = SampleDocument();

  auto buffer = MakeBuffer();
  ASSERT_NE(serializer.SerializeTyped(packet, buffer), nullptr);
  const auto decoded = serializer.DeserializeTyped(buffer);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->body, SampleDocument());
}

// nullptr is how a PacketSerializer says "drop this", so a hostile document
// costs one packet rather than the session.
TEST(JsonSerializerTest, MalformedBodyDropsThePacket) {
  JsonSerializer serializer(3);

  auto buffer = MakeNestedMsgpack(100000);
  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}

TEST(JsonSerializerTest, LimitsArePerSerializer) {
  JsonLimits limits;
  limits.max_bytes = 8;
  JsonSerializer serializer(4, limits);

  auto packet = std::make_shared<JsonPacket>(4);
  packet->body = SampleDocument();

  auto buffer = MakeBuffer();
  serializer.SerializeTyped(packet, buffer);
  EXPECT_EQ(serializer.DeserializeTyped(buffer), nullptr);
}
