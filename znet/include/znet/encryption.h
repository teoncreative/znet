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

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<HandshakePacket> packet, std::shared_ptr<Buffer> buffer) override {
    uint32_t len;
    auto* data = SerializePublicKey(packet->pub_key_, &len);
    buffer->WriteInt(len);
    buffer->Write(data, len);
    OPENSSL_free(data);
    return buffer;
  }

  std::shared_ptr<HandshakePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<HandshakePacket>();
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
  
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ConnectionReadyPacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->magic_);
    return buffer;
  }

  std::shared_ptr<ConnectionReadyPacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ConnectionReadyPacket>();
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

  std::shared_ptr<Buffer> HandleIn(std::shared_ptr<Buffer> buffer);
  std::shared_ptr<Buffer> HandleOut(std::shared_ptr<Buffer> buffer);

  void OnHandshakePacket(std::shared_ptr<HandshakePacket> packet);
  void OnAcknowledgePacket(std::shared_ptr<ConnectionReadyPacket> packet);

 private:
  PeerSession& session_;

  EVP_PKEY* pub_key_ = nullptr;
  EVP_PKEY* peer_pkey_ = nullptr;
  bool sent_handshake_ = false;
  bool sent_ready_ = false;
  bool enable_encryption_ = false;
  unsigned char* shared_secret_ = nullptr;
  size_t shared_secret_len_ = 0;
  bool key_filled_ = false;
  unsigned char* key_ = nullptr;
  size_t key_len_ = 0;

 private:
  std::shared_ptr<Buffer> HandleDecrypt(std::shared_ptr<Buffer> buffer);
  // This sends a packet that does not have any encryption nor a header that
  // gives information about its encryption, at this point we trust the
  // other side to handle it accordingly. If there is a state mismatch then
  // the packet will be handled as encrypted and fail.
  bool SendPacket(std::shared_ptr<Packet> pk);

  void SendHandshake();
  void SendReady();
};

}