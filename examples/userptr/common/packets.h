//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/packet.h"

using namespace znet;

enum PacketType : PacketId { kPacketHello, kPacketMessage };

/** @brief Client to server, once: the name this connection goes by. */
class HelloPacket : public Packet {
 public:
  HelloPacket() : Packet(kPacketHello) {}

  std::string name;
};

/** @brief Both directions: one line of chat. */
class MessagePacket : public Packet {
 public:
  MessagePacket() : Packet(kPacketMessage) {}

  std::string text;
};

class HelloSerializer : public PacketSerializer<HelloPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<HelloPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->name);
    return buffer;
  }

  std::shared_ptr<HelloPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<HelloPacket>();
    packet->name = buffer->ReadString();
    return packet;
  }
};

class MessageSerializer : public PacketSerializer<MessagePacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<MessagePacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<MessagePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<MessagePacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

inline std::shared_ptr<Codec> MakeCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketHello, std::make_unique<HelloSerializer>());
  codec->Add(kPacketMessage, std::make_unique<MessageSerializer>());
  return codec;
}
