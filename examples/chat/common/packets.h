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

class LoginRequestPacket : public Packet {
 public:
  LoginRequestPacket() : Packet(PacketId()) {}

  std::string username_;
  std::string password_;

  static PacketId PacketId() { return 1; }
};

class LoginResponsePacket : public Packet {
 public:
  LoginResponsePacket() : Packet(PacketId()) {}

  bool succeeded_ = false;
  std::string message_;
  int user_id_;

  static PacketId PacketId() { return 2; }
};

class ServerSettingsPacket : public Packet {
 public:
  ServerSettingsPacket() : Packet(PacketId()) {}

  std::vector<std::string> user_list_;

  static PacketId PacketId() { return 3; }
};

class MessagePacket : public Packet {
 public:
  MessagePacket() : Packet(PacketId()) {}

  std::string message_;
  std::string sender_username_;
  int user_id_;

  static PacketId PacketId() { return 4; }
};

class LoginRequestPacketSerializerV1
    : public PacketSerializer<LoginRequestPacket> {
 public:
  LoginRequestPacketSerializerV1() : PacketSerializer<LoginRequestPacket>() {}

  Ref<Buffer> Serialize(Ref<LoginRequestPacket> packet,
                        Ref<Buffer> buffer) override {
    buffer->WriteString(packet->username_);
    buffer->WriteString(packet->password_);
    return buffer;
  }

  Ref<LoginRequestPacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<LoginRequestPacket>();
    packet->username_ = buffer->ReadString();
    packet->password_ = buffer->ReadString();
    return packet;
  }
};

class LoginResponsePacketSerializerV1
    : public PacketSerializer<LoginResponsePacket> {
 public:
  LoginResponsePacketSerializerV1() : PacketSerializer<LoginResponsePacket>() {}

  Ref<Buffer> Serialize(Ref<LoginResponsePacket> packet,
                        Ref<Buffer> buffer) override {
    buffer->WriteBool(packet->succeeded_);
    buffer->WriteString(packet->message_);
    buffer->WriteInt(packet->user_id_);
    return buffer;
  }

  Ref<LoginResponsePacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<LoginResponsePacket>();
    packet->succeeded_ = buffer->ReadBool();
    packet->message_ = buffer->ReadString();
    packet->user_id_ = buffer->ReadInt<int>();
    return packet;
  }
};

class ServerSettingsPacketSerializerV1
    : public PacketSerializer<ServerSettingsPacket> {
 public:
  ServerSettingsPacketSerializerV1()
      : PacketSerializer<ServerSettingsPacket>() {}

  Ref<Buffer> Serialize(Ref<ServerSettingsPacket> packet,
                        Ref<Buffer> buffer) override {
    return buffer;
  }

  Ref<ServerSettingsPacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<ServerSettingsPacket>();
    return packet;
  }
};

class MessagePacketSerializerV1 : public PacketSerializer<MessagePacket> {
 public:
  MessagePacketSerializerV1() : PacketSerializer<MessagePacket>() {}

  Ref<Buffer> Serialize(Ref<MessagePacket> packet,
                        Ref<Buffer> buffer) override {
    buffer->WriteString(packet->message_);
    buffer->WriteString(packet->sender_username_);
    buffer->WriteInt(packet->user_id_);
    return buffer;
  }

  Ref<MessagePacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<MessagePacket>();
    packet->message_ = buffer->ReadString();
    packet->sender_username_ = buffer->ReadString();
    packet->user_id_ = buffer->ReadInt<int>();
    return packet;
  }
};
