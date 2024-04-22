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
  HandshakePacket() : Packet(PacketId()) { }
  ~HandshakePacket() {
    if (owner_) {
      EVP_PKEY_free(pub_key_);
    }
  }

  static PacketId PacketId() { return 10000; }

  EVP_PKEY* pub_key_;
  bool owner_; // means we have to deallocate the key
};

class HandshakePacketSerializerV1 : public PacketSerializer<HandshakePacket> {
 public:
  HandshakePacketSerializerV1() : PacketSerializer<HandshakePacket>() {}

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


class ConnectionCompletePacket : public Packet {
 public:
  ConnectionCompletePacket() : Packet(PacketId()) { }
  ~ConnectionCompletePacket() {
  }

  static PacketId PacketId() { return 10001; }

  std::string magic_;
};

class ConnectionCompletePacketSerializerV1 : public PacketSerializer<ConnectionCompletePacket> {
 public:
  ConnectionCompletePacketSerializerV1() : PacketSerializer<ConnectionCompletePacket>() {}

  Ref<Buffer> Serialize(Ref<ConnectionCompletePacket> packet, Ref<Buffer> buffer) override {
    buffer->WriteString(packet->magic_);
    return buffer;
  }

  Ref<ConnectionCompletePacket> Deserialize(Ref<Buffer> buffer) override {
    auto packet = CreateRef<ConnectionCompletePacket>();
    packet->magic_ = buffer->ReadString();
    return packet;
  }
};

class ConnectionSession;

class EncryptionLayer {
 public:
  EncryptionLayer(ConnectionSession& session);
  ~EncryptionLayer();

  void Initialize(bool send);

  Ref<Buffer> HandleIn(Ref<Buffer> buffer);

  Ref<Buffer> HandleOut(Ref<Buffer> buffer);

  void OnHandshakePacket(ConnectionSession& session, Ref<HandshakePacket> packet);
  void OnAcknowledgePacket(ConnectionSession& session, Ref<ConnectionCompletePacket> packet);

 private:
  ConnectionSession& session_;
  HandlerLayer handler_layer_;

  EVP_PKEY* pub_key_ = nullptr;
  EVP_PKEY* peer_pkey_ = nullptr;
  bool sent_handshake_ = false;
  bool sent_ack_ = false;
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
  void SendAcknowledge();
};

}