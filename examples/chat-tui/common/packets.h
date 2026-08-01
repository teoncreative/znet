//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Wire format for the chat-tui example.
//
// Five packets, three of them one field. The room a message belongs to travels
// in the packet rather than being implied by a channel, so the server stays the
// only thing that knows who is in which room.
//

#pragma once

#include "znet/codec.h"
#include "znet/packet.h"

#include <string>
#include <vector>

using namespace znet;

enum PacketType : PacketId {
  kPacketWelcome,
  kPacketSelectRoom,
  kPacketChat,
  kPacketSystem,
};

/**
 * @brief Server to client, once on connect: your name, and every room.
 *
 * The client does not choose its own name and never sends one, so there is no
 * join handshake: the server assigns at connect and says so here. That is what
 * makes names unique, and what lets a client recognise its own messages.
 */
class WelcomePacket : public Packet {
 public:
  WelcomePacket() : Packet(kPacketWelcome) {}

  std::string name;
  std::vector<std::string> rooms;
};

/** @brief Client to server: put me in this room, take me out of the old one. */
class SelectRoomPacket : public Packet {
 public:
  SelectRoomPacket() : Packet(kPacketSelectRoom) {}

  std::string room;
};

/** @brief Both directions: one line of chat, tagged with its room. */
class ChatPacket : public Packet {
 public:
  ChatPacket() : Packet(kPacketChat) {}

  std::string room;
  std::string author;
  std::string text;
};

/** @brief Server to client: a notice, rendered differently from chat. */
class SystemPacket : public Packet {
 public:
  SystemPacket() : Packet(kPacketSystem) {}

  std::string room;
  std::string text;
};

class WelcomeSerializer : public PacketSerializer<WelcomePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<WelcomePacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->name);
    buffer->WriteVarInt<uint32_t>(static_cast<uint32_t>(packet->rooms.size()));
    for (const std::string& room : packet->rooms) {
      buffer->WriteString(room);
    }
    return buffer;
  }

  std::shared_ptr<WelcomePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<WelcomePacket>();
    packet->name = buffer->ReadString();
    uint32_t count = buffer->ReadVarInt<uint32_t>();
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;
    }
    // A count read off the wire is attacker-controlled, so cap it before it is
    // used to size anything.
    if (count > kMaxRooms) {
      return nullptr;
    }
    for (uint32_t i = 0; i < count; i++) {
      packet->rooms.push_back(buffer->ReadString());
      if (buffer->GetAndClearLastError() != BufferError::None) {
        return nullptr;
      }
    }
    return packet;
  }

 private:
  static const uint32_t kMaxRooms = 256;
};

class SelectRoomSerializer : public PacketSerializer<SelectRoomPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<SelectRoomPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->room);
    return buffer;
  }

  std::shared_ptr<SelectRoomPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<SelectRoomPacket>();
    packet->room = buffer->ReadString();
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;
    }
    return packet;
  }
};

class ChatSerializer : public PacketSerializer<ChatPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ChatPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->room);
    buffer->WriteString(packet->author);
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<ChatPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ChatPacket>();
    packet->room = buffer->ReadString();
    packet->author = buffer->ReadString();
    packet->text = buffer->ReadString();
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;
    }
    return packet;
  }
};

class SystemSerializer : public PacketSerializer<SystemPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<SystemPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->room);
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<SystemPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<SystemPacket>();
    packet->room = buffer->ReadString();
    packet->text = buffer->ReadString();
    if (buffer->GetAndClearLastError() != BufferError::None) {
      return nullptr;
    }
    return packet;
  }
};

/** @brief One codec, shared by every session: serializers hold no state. */
inline std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketWelcome, std::make_unique<WelcomeSerializer>());
  codec->Add(kPacketSelectRoom, std::make_unique<SelectRoomSerializer>());
  codec->Add(kPacketChat, std::make_unique<ChatSerializer>());
  codec->Add(kPacketSystem, std::make_unique<SystemSerializer>());
  return codec;
}
