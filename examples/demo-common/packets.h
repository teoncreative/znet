//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/znet.h"

using namespace znet;

class DemoPacket : public Packet {
 public:
  DemoPacket() : Packet(1) { }

  char text[4000];
};

class DemoPacketSerializerV1 : public PacketSerializer<DemoPacket> {
 public:
  DemoPacketSerializerV1() : PacketSerializer<DemoPacket>(1) {}

  Ref<Buffer> Serialize(Ref<DemoPacket> packet, Ref<Buffer> buffer) override {
    buffer->Write(packet->text, 4000);
    return buffer;
  }

  Ref<DemoPacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<DemoPacket>();
    buffer->Read(packet->text, 4000);
    return packet;
  }
};
