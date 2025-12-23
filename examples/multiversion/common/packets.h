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

#include "znet/codec.h"
#include "znet/packet_handler.h"
#include "znet/types.h"
#include "types.h"

using namespace znet;

enum PacketType : PacketId {
  PACKET_NETWORK_SETTINGS,
  PACKET_START_GAME,
  PACKET_CLIENT_READY,
  PACKET_MOVE,
  PACKET_TELEPORT,

};

void WriteVec3(std::shared_ptr<Buffer> buffer, const Vec3& pos) {
  buffer->WriteDouble(pos.x);
  buffer->WriteDouble(pos.y);
  buffer->WriteDouble(pos.z);
}

Vec3 ReadVec3(std::shared_ptr<Buffer> buffer) {
  return {buffer->ReadDouble(), buffer->ReadDouble(), buffer->ReadDouble()};
}

class NetworkSettingsPacket : public Packet {
 public:
  NetworkSettingsPacket() : Packet(PACKET_NETWORK_SETTINGS) { }

  int protocol_;

};

class NetworkSettingsSerializer_v1 : public PacketSerializer<NetworkSettingsPacket> {
 public:
  NetworkSettingsSerializer_v1() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<NetworkSettingsPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<int>(packet->protocol_);
    return buffer;
  }

  std::shared_ptr<NetworkSettingsPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<NetworkSettingsPacket>();
    packet->protocol_ = buffer->ReadInt<int>();
    return packet;
  }
};

// Spawns the player
class StartGamePacket : public Packet {
 public:
  StartGamePacket() : Packet(PACKET_START_GAME) { }

  std::string level_name_;
  int game_mode_;
  // Added in v2
  Vec3 spawn_pos_;
};

// Sent to server from client
// Moves the player by the delta amount
class MovePacket : public Packet {
 public:
  MovePacket() : Packet(PACKET_MOVE) { }

  Vec3 delta;
};

// Sent to client from server
// Teleports the player to a position
class TeleportPacket : public Packet {
 public:
  TeleportPacket() : Packet(PACKET_TELEPORT) { }

  Vec3 pos;
};

class ClientReadyPacket : public Packet {
 public:
  ClientReadyPacket() : Packet(PACKET_CLIENT_READY) { }

};

class StartGameSerializer_v1 : public PacketSerializer<StartGamePacket> {
 public:
  StartGameSerializer_v1() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<StartGamePacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->level_name_);
    buffer->WriteInt<int>(packet->game_mode_);
    return buffer;
  }

  std::shared_ptr<StartGamePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<StartGamePacket>();
    packet->level_name_ = buffer->ReadString();
    packet->game_mode_ = buffer->ReadInt<int>();
    return packet;
  }
};

class StartGameSerializer_v2 : public StartGameSerializer_v1 {
 public:
  StartGameSerializer_v2() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<StartGamePacket> packet, std::shared_ptr<Buffer> buffer) override {
    StartGameSerializer_v1::SerializeTyped(packet, buffer);
    WriteVec3(buffer, packet->spawn_pos_);
    return buffer;
  }

  std::shared_ptr<StartGamePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = StartGameSerializer_v1::DeserializeTyped(buffer);
    packet->spawn_pos_ = ReadVec3(buffer);
    return packet;
  }
};

class MoveSerializer_v1 : public PacketSerializer<MovePacket> {
 public:
  MoveSerializer_v1() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<MovePacket> packet, std::shared_ptr<Buffer> buffer) override {
    WriteVec3(buffer, packet->delta);
    return buffer;
  }

  std::shared_ptr<MovePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<MovePacket>();
    packet->delta = ReadVec3(buffer);
    return packet;
  }
};

class ClientReadySerializer_v1 : public PacketSerializer<ClientReadyPacket> {
 public:
  ClientReadySerializer_v1() {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ClientReadyPacket> packet, std::shared_ptr<Buffer> buffer) override {
    return buffer;
  }

  std::shared_ptr<ClientReadyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ClientReadyPacket>();
    return packet;
  }
};

struct Codecs {
  std::shared_ptr<Codec> codec_v1;
  std::shared_ptr<Codec> codec_v2;

  std::shared_ptr<Codec> codec_latest;

  Codecs() {
    codec_v1 = std::make_shared<Codec>();
    codec_v1->Add(PACKET_NETWORK_SETTINGS,
                  std::make_unique<NetworkSettingsSerializer_v1>());
    codec_v1->Add(PACKET_START_GAME, std::make_unique<StartGameSerializer_v1>());
    codec_v1->Add(PACKET_CLIENT_READY, std::make_unique<ClientReadySerializer_v1>());
    codec_v1->Add(PACKET_MOVE, std::make_unique<MoveSerializer_v1>());

    codec_v2 = std::make_shared<Codec>();
    codec_v2->Add(PACKET_NETWORK_SETTINGS,
                  std::make_unique<NetworkSettingsSerializer_v1>());
    codec_v2->Add(PACKET_START_GAME, std::make_unique<StartGameSerializer_v2>());
    codec_v2->Add(PACKET_CLIENT_READY, std::make_unique<ClientReadySerializer_v1>());
    codec_v2->Add(PACKET_MOVE, std::make_unique<MoveSerializer_v1>());

    codec_latest = codec_v2;
  }
};
