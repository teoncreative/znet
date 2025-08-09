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
// Created by Metehan Gezer on 08/08/2025.
//

#ifndef ZNET_PARENT_RELAY_H
#define ZNET_PARENT_RELAY_H

#include "znet/server.h"

namespace znet {
namespace p2p {

// Relay flow: (C1 is the first client, C2 is the second client, S is the server)
// IdentifyPacket C1 -> S
// SetPeerNamePacket S -> C1 - Relay server selects a unique name and replies
//
// IdentifyPacket C2 -> S
// SetPeerNamePacket S -> C2 - Relay server selects a unique name and replies
//
// ConnectPeerPacket C1 -> S - C1 asks to connect to C2's peer name
// ConnectPeerPacket C2 -> S - C2 asks to connect to C1's peer name
//
// When the server sees that two peers want to connect to each other,
// it will send these packets with each others' information
//
// StartPunchPacket S -> C1
// StartPunchPacket S -> C2


enum PacketType {
  PACKET_IDENTIFY,
  PACKET_SET_PEER_NAME,
  PACKET_CONNECT_PEER,
  PACKET_START_PUNCH_REQUEST,
};

class IdentifyPacket : public Packet {
 public:
  IdentifyPacket() : Packet(PACKET_IDENTIFY) {}

  PortNumber port_;
};

class SetPeerNamePacket : public Packet {
 public:
  SetPeerNamePacket() : Packet(PACKET_SET_PEER_NAME) {}

  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};

class ConnectPeerPacket : public Packet {
 public:
  ConnectPeerPacket() : Packet(PACKET_CONNECT_PEER) {}

  std::string target_peer_;
};

class StartPunchRequestPacket : public Packet {
 public:
  StartPunchRequestPacket() : Packet(PACKET_START_PUNCH_REQUEST) {}

  std::string target_peer_;
  PortNumber bind_port_;
  std::shared_ptr<InetAddress> target_endpoint_;
};

class IdentifySerializer : public PacketSerializer<IdentifyPacket> {
 public:
  IdentifySerializer() : PacketSerializer<IdentifyPacket>() {}
  ~IdentifySerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<IdentifyPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WritePort(packet->port_);
    return buffer;
  }

  std::shared_ptr<IdentifyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<IdentifyPacket>();
    packet->port_ = buffer->ReadPort();
    return packet;
  }
};

class SetPeerNameSerializer : public PacketSerializer<SetPeerNamePacket> {
 public:
  SetPeerNameSerializer() : PacketSerializer<SetPeerNamePacket>() {}
  ~SetPeerNameSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<SetPeerNamePacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->peer_name_);
    buffer->WriteInetAddress(*packet->endpoint_);
    return buffer;
  }

  std::shared_ptr<SetPeerNamePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<SetPeerNamePacket>();
    packet->peer_name_ = buffer->ReadString();
    packet->endpoint_ = buffer->ReadInetAddress();
    return packet;
  }
};

class ConnectPeerSerializer : public PacketSerializer<ConnectPeerPacket> {
 public:
  ConnectPeerSerializer() : PacketSerializer<ConnectPeerPacket>() {}
  ~ConnectPeerSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ConnectPeerPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_peer_);
    return buffer;
  }

  std::shared_ptr<ConnectPeerPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ConnectPeerPacket>();
    packet->target_peer_ = buffer->ReadString();
    return packet;
  }
};

class StartPunchRequestSerializer : public PacketSerializer<StartPunchRequestPacket> {
 public:
  StartPunchRequestSerializer() : PacketSerializer<StartPunchRequestPacket>() {}
  ~StartPunchRequestSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<StartPunchRequestPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_peer_);
    buffer->WritePort(packet->bind_port_);
    buffer->WriteInetAddress(*packet->target_endpoint_);
    return buffer;
  }

  std::shared_ptr<StartPunchRequestPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<StartPunchRequestPacket>();
    packet->target_peer_ = buffer->ReadString();
    packet->bind_port_ = buffer->ReadPort();
    packet->target_endpoint_ = buffer->ReadInetAddress();
    return packet;
  }
};

inline std::shared_ptr<Codec> BuildCodec() {
  std::shared_ptr<znet::Codec> codec = std::make_shared<znet::Codec>();
  codec->Add(PACKET_IDENTIFY, std::make_unique<IdentifySerializer>());
  codec->Add(PACKET_SET_PEER_NAME, std::make_unique<SetPeerNameSerializer>());
  codec->Add(PACKET_CONNECT_PEER, std::make_unique<ConnectPeerSerializer>());
  codec->Add(PACKET_START_PUNCH_REQUEST, std::make_unique<StartPunchRequestSerializer>());
  return std::move(codec);
}

}
}

#endif  //ZNET_PARENT_RELAY_H
