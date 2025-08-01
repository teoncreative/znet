//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef RELAY_SERVER_H
#define RELAY_SERVER_H

#include "znet/server.h"

namespace znet {
namespace holepunch {

enum PacketType {
  // A peer requests to register themselves
  PACKET_REGISTER_PEER,
  // Response to PACKET_REGISTER_PEER
  PACKET_REGISTER_PEER_RESPONSE,
  // A peer requests to connect to another peer
  PACKET_CONNECT_PEER,
  // Response to PACKET_CONNECT_PEER
  PACKET_CONNECT_PEER_RESPONSE,
};

class RegisterPeerPacket : public Packet {
 public:
  RegisterPeerPacket() : Packet(PACKET_REGISTER_PEER) {}

  std::string peer_name_;
  PortNumber port_;
};

enum RegisterPeerStatus {
  Success,
  AlreadyRegistered,
};

class RegisterPeerResponsePacket : public Packet {
 public:
  RegisterPeerResponsePacket() : Packet(PACKET_REGISTER_PEER_RESPONSE) {}

  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_; // self endpoint
  bool ok_ = false;
};

class ConnectPeerPacket : public Packet {
 public:
  ConnectPeerPacket() : Packet(PACKET_CONNECT_PEER) {}

  std::string target_;
};

class ConnectPeerResponsePacket : public Packet {
 public:
  ConnectPeerResponsePacket() : Packet(PACKET_CONNECT_PEER_RESPONSE) {}

  std::string target_;
  std::shared_ptr<InetAddress> endpoint_;
};

class RegisterPeerSerializer : public PacketSerializer<RegisterPeerPacket> {
 public:
  RegisterPeerSerializer() : PacketSerializer<RegisterPeerPacket>() {}
  ~RegisterPeerSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<RegisterPeerPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->peer_name_);
    buffer->WritePort(packet->port_);
    return buffer;
  }

  std::shared_ptr<RegisterPeerPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<RegisterPeerPacket>();
    packet->peer_name_ = buffer->ReadString();
    packet->port_ = buffer->ReadPort();
    return packet;
  }
};

class RegisterPeerResponseSerializer : public PacketSerializer<RegisterPeerResponsePacket> {
 public:
  RegisterPeerResponseSerializer() : PacketSerializer<RegisterPeerResponsePacket>() {}
  ~RegisterPeerResponseSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<RegisterPeerResponsePacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->peer_name_);
    buffer->WriteBool(packet->ok_);
    return buffer;
  }

  std::shared_ptr<RegisterPeerResponsePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<RegisterPeerResponsePacket>();
    packet->peer_name_ = buffer->ReadString();
    packet->ok_ = buffer->ReadBool();
    return packet;
  }
};

class ConnectPeerSerializer : public PacketSerializer<ConnectPeerPacket> {
 public:
  ConnectPeerSerializer() : PacketSerializer<ConnectPeerPacket>() {}
  ~ConnectPeerSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ConnectPeerPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_);
    return buffer;
  }

  std::shared_ptr<ConnectPeerPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ConnectPeerPacket>();
    packet->target_ = buffer->ReadString();
    return packet;
  }
};

class ConnectPeerResponseSerializer : public PacketSerializer<ConnectPeerResponsePacket> {
 public:
  ConnectPeerResponseSerializer() : PacketSerializer<ConnectPeerResponsePacket>() {}
  ~ConnectPeerResponseSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ConnectPeerResponsePacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_);
    buffer->WriteInetAddress(*packet->endpoint_);
    return buffer;
  }

  std::shared_ptr<ConnectPeerResponsePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ConnectPeerResponsePacket>();
    packet->target_ = buffer->ReadString();
    packet->endpoint_ = buffer->ReadInetAddress();
    return packet;
  }
};

std::shared_ptr<Codec> BuildCodec() {
  std::shared_ptr<znet::Codec> codec = std::make_shared<znet::Codec>();
  codec->Add(znet::holepunch::PACKET_REGISTER_PEER, std::make_unique<znet::holepunch::RegisterPeerSerializer>());
  codec->Add(znet::holepunch::PACKET_REGISTER_PEER_RESPONSE, std::make_unique<znet::holepunch::RegisterPeerResponseSerializer>());
  codec->Add(znet::holepunch::PACKET_CONNECT_PEER, std::make_unique<znet::holepunch::ConnectPeerSerializer>());
  codec->Add(znet::holepunch::PACKET_CONNECT_PEER_RESPONSE, std::make_unique<znet::holepunch::ConnectPeerResponseSerializer>());
  return std::move(codec);
}

}
}


#endif //RELAY_SERVER_H
