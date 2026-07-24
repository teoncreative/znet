//
//    Copyright 2026 Metehan Gezer
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

enum PacketType { PACKET_DEMO };

// A tiny text packet used by the ZDT example to demonstrate reliable, ordered
// delivery over UDP.
class DemoPacket : public Packet {
 public:
  DemoPacket() : Packet(PACKET_DEMO) {}

  std::string text;
};

class DemoSerializer : public PacketSerializer<DemoPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<DemoPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<DemoPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<DemoPacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};
