//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_P2P_RENDEZVOUS_H_
#define ZNET_P2P_RENDEZVOUS_H_

#include "znet/server.h"

namespace znet {
namespace p2p {

// Rendezvous flow: (C1 is the first client, C2 is the second client, S is the server)
// IdentifyPacket C1 -> S
// SetPeerNamePacket S -> C1 - Rendezvous server selects a unique name and replies
//
// IdentifyPacket C2 -> S
// SetPeerNamePacket S -> C2 - Rendezvous server selects a unique name and replies
//
// ConnectPeerPacket C1 -> S - C1 asks to connect to C2's peer name
// ConnectPeerPacket C2 -> S - C2 asks to connect to C1's peer name
//
// A ConnectPeerPacket naming a peer the server does not know is answered with
// PeerNotFoundPacket. Asking is not queued: if the target identifies later,
// the asker has to ask again.
//
// When the server sees that two peers want to connect to each other,
// it will send these packets with each others' information
//
// StartPunchRequestPacket S -> C1
// StartPunchRequestPacket S -> C2

// wire ids of the rendezvous protocol; plain PacketIds, usable directly with
// Codec::Add()
ZNET_INLINE_CONSTEXPR PacketId kPacketIdentify = 0;
ZNET_INLINE_CONSTEXPR PacketId kPacketSetPeerName = 1;
ZNET_INLINE_CONSTEXPR PacketId kPacketConnectPeer = 2;
ZNET_INLINE_CONSTEXPR PacketId kPacketStartPunchRequest = 3;
ZNET_INLINE_CONSTEXPR PacketId kPacketPeerNotFound = 4;

// Cap on the private endpoints a peer may claim
ZNET_INLINE_CONSTEXPR size_t kMaxPrivateEndpoints = 32;

namespace detail {

inline void WriteEndpoints(
    const std::shared_ptr<Buffer>& buffer,
    const std::vector<std::shared_ptr<InetAddress>>& endpoints) {
  size_t count = endpoints.size();
  if (count > kMaxPrivateEndpoints) {
    count = kMaxPrivateEndpoints;
  }
  buffer->WriteInt<uint8_t>(static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; i++) {
    buffer->WriteInetAddress(*endpoints[i]);
  }
}

// Returns false on a corrupt address, so the caller can refuse the frame.
inline bool ReadEndpoints(const std::shared_ptr<Buffer>& buffer,
                          std::vector<std::shared_ptr<InetAddress>>& out) {
  const uint8_t count = buffer->ReadInt<uint8_t>();
  if (count > kMaxPrivateEndpoints) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    auto address = buffer->ReadInetAddress();
    if (!address) {
      return false;
    }
    out.push_back(std::move(address));
  }
  return true;
}

}  // namespace detail

class IdentifyPacket : public Packet {
 public:
  IdentifyPacket() : Packet(kPacketIdentify) {}

  // every address the client sees itself at, e.g. 192.168.1.5:54321. The
  // server relays them to the matched peer as punch candidates, which is what
  // lets two peers on a shared network reach each other. A multi-homed host
  // cannot know which one the peer shares, so it offers all of them. Optional:
  // peers that send none just punch on the public endpoint alone.
  std::vector<std::shared_ptr<InetAddress>> local_endpoints_;
  // the local port this client punches from: the relay connection's own port
  // for the one-shot locator, the Host's UDP port for a mesh. The server
  // pairs it with the address it observed. Note the assumption both cases
  // share: the NAT maps local port N to public port N.
  PortNumber punch_port_ = 0;
};

class SetPeerNamePacket : public Packet {
 public:
  SetPeerNamePacket() : Packet(kPacketSetPeerName) {}

  std::string peer_name_;
  std::shared_ptr<InetAddress> endpoint_;
};

class ConnectPeerPacket : public Packet {
 public:
  ConnectPeerPacket() : Packet(kPacketConnectPeer) {}

  std::string target_peer_;
};

class StartPunchRequestPacket : public Packet {
 public:
  StartPunchRequestPacket() : Packet(kPacketStartPunchRequest) {}

  std::string target_peer_;
  std::shared_ptr<InetAddress> target_endpoint_;
  // the peer's claimed private addresses, minus any matching what the server
  // observed; further punch candidates
  std::vector<std::shared_ptr<InetAddress>> target_private_endpoints_;
  // the server picks the punch transport, so both peers always agree
  ConnectionType connection_type_ = ConnectionType::ZDT;
  uint64_t punch_id_ = 0;
};

class PeerNotFoundPacket : public Packet {
 public:
  PeerNotFoundPacket() : Packet(kPacketPeerNotFound) {}

  std::string target_peer_;
};

class IdentifySerializer : public PacketSerializer<IdentifyPacket> {
 public:
  IdentifySerializer() : PacketSerializer<IdentifyPacket>() {}
  ~IdentifySerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<IdentifyPacket> packet, std::shared_ptr<Buffer> buffer) override {
    detail::WriteEndpoints(buffer, packet->local_endpoints_);
    buffer->WriteInt<uint16_t>(static_cast<uint16_t>(packet->punch_port_));
    return buffer;
  }

  std::shared_ptr<IdentifyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<IdentifyPacket>();
    if (!detail::ReadEndpoints(buffer, packet->local_endpoints_)) {
      return nullptr;  // claimed an address, then sent a corrupt one
    }
    packet->punch_port_ = buffer->ReadInt<uint16_t>();
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
    if (!packet->endpoint_) {
      return nullptr;  // corrupt address; refuse the frame, not the process
    }
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
    buffer->WriteInetAddress(*packet->target_endpoint_);
    detail::WriteEndpoints(buffer, packet->target_private_endpoints_);
    buffer->WriteInt<uint64_t>(packet->punch_id_);
    buffer->WriteInt<uint8_t>(static_cast<uint8_t>(packet->connection_type_));
    return buffer;
  }

  std::shared_ptr<StartPunchRequestPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<StartPunchRequestPacket>();
    packet->target_peer_ = buffer->ReadString();
    packet->target_endpoint_ = buffer->ReadInetAddress();
    if (!packet->target_endpoint_) {
      return nullptr;  // corrupt address; refuse the frame, not the process
    }
    if (!detail::ReadEndpoints(buffer, packet->target_private_endpoints_)) {
      return nullptr;
    }
    packet->punch_id_ = buffer->ReadInt<uint64_t>();
    const uint8_t raw_type = buffer->ReadInt<uint8_t>();
    // the type dispatches a punch; an unknown one is not worth guessing about
    if (raw_type == static_cast<uint8_t>(ConnectionType::TCP)) {
      packet->connection_type_ = ConnectionType::TCP;
    } else if (raw_type == static_cast<uint8_t>(ConnectionType::ZDT)) {
      packet->connection_type_ = ConnectionType::ZDT;
    } else {
      return nullptr;
    }
    return packet;
  }
};

class PeerNotFoundSerializer : public PacketSerializer<PeerNotFoundPacket> {
 public:
  PeerNotFoundSerializer() : PacketSerializer<PeerNotFoundPacket>() {}
  ~PeerNotFoundSerializer() override = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<PeerNotFoundPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->target_peer_);
    return buffer;
  }

  std::shared_ptr<PeerNotFoundPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<PeerNotFoundPacket>();
    packet->target_peer_ = buffer->ReadString();
    return packet;
  }
};

inline std::shared_ptr<Codec> BuildRendezvousCodec() {
  std::shared_ptr<znet::Codec> codec = std::make_shared<znet::Codec>();
  codec->Add(kPacketIdentify, std::make_unique<IdentifySerializer>());
  codec->Add(kPacketSetPeerName, std::make_unique<SetPeerNameSerializer>());
  codec->Add(kPacketConnectPeer, std::make_unique<ConnectPeerSerializer>());
  codec->Add(kPacketStartPunchRequest, std::make_unique<StartPunchRequestSerializer>());
  codec->Add(kPacketPeerNotFound, std::make_unique<PeerNotFoundSerializer>());
  return codec;
}

}  // namespace p2p
}  // namespace znet


#endif  // ZNET_P2P_RENDEZVOUS_H_
