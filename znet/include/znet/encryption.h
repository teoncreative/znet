//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "base/packet.h"
#include "logger.h"
#include "packet_handler.h"

#include <openssl/dh.h>
#include <openssl/engine.h>
#include <openssl/bn.h>

namespace znet {

unsigned char* SerializePublicKey(EVP_PKEY* pkey, uint32_t* len);

EVP_PKEY* DeserializePublicKey(const unsigned char* der, int len);

class HandshakePacket : public Packet {
 public:
  HandshakePacket() : Packet(GetPacketId()) { }
  ~HandshakePacket() {
    if (owner_) {
      EVP_PKEY_free(pub_key_);
    }
  }

  static PacketId GetPacketId() { return -1; }

  EVP_PKEY* pub_key_;
  bool owner_; // means we have to deallocate the key
};

class HandshakePacketSerializerV1 : public PacketSerializer<HandshakePacket> {
 public:
  HandshakePacketSerializerV1() : PacketSerializer<HandshakePacket>() {}
  ~HandshakePacketSerializerV1() = default;

  Ref<Buffer> Serialize(Ref<HandshakePacket> packet, Ref<Buffer> buffer) override {
    uint32_t len;
    auto* data = SerializePublicKey(packet->pub_key_, &len);
    buffer->WriteInt(len);
    buffer->Write(data, len);
    OPENSSL_free(data);
    return buffer;
  }

  Ref<HandshakePacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<HandshakePacket>();
    auto len = buffer->ReadInt<uint32_t>();
    auto* data = new unsigned char[len];
    buffer->Read(data, len);
    packet->pub_key_ = DeserializePublicKey(data, len);
    packet->owner_ = true;
    delete[] data;
    return packet;
  }
};


class ConnectionReadyPacket : public Packet {
 public:
  ConnectionReadyPacket() : Packet(GetPacketId()) { }
  ~ConnectionReadyPacket() = default;

  static PacketId GetPacketId() { return -2; }

  std::string magic_;
};

class ConnectionReadyPacketSerializerV1
    : public PacketSerializer<ConnectionReadyPacket> {
 public:
  ConnectionReadyPacketSerializerV1() : PacketSerializer<ConnectionReadyPacket>() {}
  ~ConnectionReadyPacketSerializerV1() = default;
  
  Ref<Buffer> Serialize(Ref<ConnectionReadyPacket> packet, Ref<Buffer> buffer) override {
    buffer->WriteString(packet->magic_);
    return buffer;
  }

  Ref<ConnectionReadyPacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<ConnectionReadyPacket>();
    packet->magic_ = buffer->ReadString();
    return packet;
  }
};

class PeerSession;

class EncryptionLayer {
 public:
  EncryptionLayer(PeerSession& session);
  ~EncryptionLayer();

  void Initialize(bool send);

  Ref<Buffer> HandleIn(Ref<Buffer> buffer);
  Ref<Buffer> HandleOut(Ref<Buffer> buffer);

  void OnHandshakePacket(PeerSession& session, Ref<HandshakePacket> packet);
  void OnAcknowledgePacket(PeerSession& session, Ref<ConnectionReadyPacket> packet);

 private:
  PeerSession& session_;
  HandlerLayer handler_layer_;

  EVP_PKEY* pub_key_ = nullptr;
  EVP_PKEY* peer_pkey_ = nullptr;
  bool sent_handshake_ = false;
  bool sent_ready_ = false;
  bool enable_encryption_ = false;
  bool handoff_ = false;
  unsigned char* shared_secret_ = nullptr;
  size_t shared_secret_len_ = 0;
  bool key_filled_ = false;
  unsigned char* key_ = nullptr;
  size_t key_len_ = 0;

 private:
  Ref<Buffer> HandleDecrypt(Ref<Buffer> buffer);

  void SendHandshake();
  void SendReady();
};

}