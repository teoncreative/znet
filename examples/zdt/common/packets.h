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
// Three message kinds, each wanting a different delivery guarantee. This is
// what ZDT offers over a plain reliable stream, so the example sends all three
// at once and the server reports what each one actually did.
//

#pragma once

#include "znet/packet.h"
#include "znet/send_options.h"

using namespace znet;

enum PacketType : PacketId { kPacketChat = 1, kPacketPosition = 2, kPacketChunk = 3 };

// Channels have independent sequence spaces, so an ordered message on one never
// waits behind an ordered message on another. Keeping the bulk transfer off the
// chat channel is the whole reason to use more than one.
enum Channel : uint8_t { kChatChannel = 0, kPositionChannel = 1, kChunkChannel = 2 };

// both ends need this: the client sends this many, the server reports when it
// has them all
constexpr uint32_t kChunkCount = 32;

/** @brief Reliable and ordered: nothing lost, nothing reordered. */
inline SendOptions ChatOptions() {
  SendOptions options;
  options.Set<ChannelKey>(kChatChannel);
  return options;  // reliable and ordered are the defaults
}

/**
 * @brief Unreliable and unordered: never retransmitted, delivered on arrival.
 *
 * The right choice when a newer message makes an older one irrelevant. Waiting
 * for a lost position update only delays the one that supersedes it.
 */
inline SendOptions PositionOptions() {
  SendOptions options;
  options.Set<ReliableKey>(false);
  options.Set<OrderedKey>(false);
  options.Set<ChannelKey>(kPositionChannel);
  return options;
}

/**
 * @brief Reliable but unordered: nothing lost, delivered as it arrives.
 *
 * Chunks are self-describing, so holding one back to wait for its predecessor
 * would stall the transfer for no benefit.
 */
inline SendOptions ChunkOptions() {
  SendOptions options;
  options.Set<OrderedKey>(false);
  options.Set<ChannelKey>(kChunkChannel);
  return options;
}

class ChatPacket : public Packet {
 public:
  ChatPacket() : Packet(kPacketChat) {}

  std::string text;
};

/** @brief A position sample, tagged with the tick it was taken on. */
class PositionPacket : public Packet {
 public:
  PositionPacket() : Packet(kPacketPosition) {}

  uint32_t tick = 0;
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

/** @brief One piece of a larger transfer, identified by index. */
class ChunkPacket : public Packet {
 public:
  ChunkPacket() : Packet(kPacketChunk) {}

  uint32_t index = 0;
  std::string payload;
};

class ChatSerializer : public PacketSerializer<ChatPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ChatPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<ChatPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ChatPacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

class PositionSerializer : public PacketSerializer<PositionPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<PositionPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->tick);
    buffer->WriteFloat(packet->x);
    buffer->WriteFloat(packet->y);
    buffer->WriteFloat(packet->z);
    return buffer;
  }

  std::shared_ptr<PositionPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<PositionPacket>();
    packet->tick = buffer->ReadInt<uint32_t>();
    packet->x = buffer->ReadFloat();
    packet->y = buffer->ReadFloat();
    packet->z = buffer->ReadFloat();
    // anything off the network can be truncated; refusing beats guessing
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;
    }
    return packet;
  }
};

class ChunkSerializer : public PacketSerializer<ChunkPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ChunkPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint32_t>(packet->index);
    buffer->WriteString(packet->payload);
    return buffer;
  }

  std::shared_ptr<ChunkPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ChunkPacket>();
    packet->index = buffer->ReadInt<uint32_t>();
    packet->payload = buffer->ReadString();
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;
    }
    return packet;
  }
};

/** @brief Registers all three, the same way on both ends. */
inline std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketChat, std::make_unique<ChatSerializer>());
  codec->Add(kPacketPosition, std::make_unique<PositionSerializer>());
  codec->Add(kPacketChunk, std::make_unique<ChunkSerializer>());
  return codec;
}
