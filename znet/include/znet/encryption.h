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

#include "znet/compression.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/precompiled.h"

#include <openssl/dh.h>
#include <openssl/engine.h>

namespace znet {

struct PKeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept {
    EVP_PKEY_free(p);
  }
};
using UniquePKey = std::unique_ptr<EVP_PKEY, PKeyDeleter>;

unsigned char* SerializePublicKey(EVP_PKEY* pkey, uint32_t* len);

UniquePKey DeserializePublicKey(const unsigned char* der, uint32_t len);

UniquePKey CloneKey(const UniquePKey& k);

class HandshakePacket : public Packet {
 public:
  HandshakePacket() : Packet(GetPacketId()) { }
  ~HandshakePacket() { }

  static PacketId GetPacketId() { return static_cast<PacketId>(-2); }

  UniquePKey pub_key_ = nullptr;
  // session parameters, chosen by the server. only meaningful on the packet the
  // server sends; the initiator's copy carries defaults and is ignored.
  bool encryption_ = true;
  CompressionTypeRaw compression_ = 0;
};

class HandshakePacketSerializerV1 : public PacketSerializer<HandshakePacket> {
 public:
  HandshakePacketSerializerV1() : PacketSerializer<HandshakePacket>() {}
  ~HandshakePacketSerializerV1() = default;

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<HandshakePacket> packet, std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint8_t>(packet->encryption_ ? 1 : 0);
    buffer->WriteInt<CompressionTypeRaw>(packet->compression_);

    uint32_t len = 0;
    auto* data = SerializePublicKey(packet->pub_key_.get(), &len);

    buffer->WriteInt<uint32_t>(len);
    if (len > 0) {
      buffer->Write(data, len);
      OPENSSL_free(data);
    }
    return buffer;
  }

  std::shared_ptr<HandshakePacket> DeserializeTyped(std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<HandshakePacket>();
    packet->encryption_ = buffer->ReadInt<uint8_t>() != 0;
    packet->compression_ = buffer->ReadInt<CompressionTypeRaw>();
    if (uint32_t len = buffer->ReadInt<uint32_t>()) {
      std::vector<unsigned char> tmp(len);
      buffer->Read(tmp.data(), len);
      packet->pub_key_ = DeserializePublicKey(tmp.data(), len);
    }
    return packet;
  }
};


class ConnectionReadyPacket : public Packet {
 public:
  ConnectionReadyPacket() : Packet(GetPacketId()) { }
  ~ConnectionReadyPacket() = default;

  static PacketId GetPacketId() { return static_cast<PacketId>(-3); }

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

  /**
   * @brief Starts the key exchange.
   *
   * `want_encryption` is the server's policy and is only read on the accepting
   * side. The initiator always offers a key and then follows whatever the
   * server selects, so a client needs no matching configuration.
   */
  void Initialize(bool send, bool want_encryption = true);

  std::shared_ptr<Buffer> HandleIn(std::shared_ptr<Buffer> buffer);
  std::shared_ptr<Buffer> HandleOut(std::shared_ptr<Buffer> buffer);

  void OnHandshakePacket(std::shared_ptr<HandshakePacket> packet);
  void OnAcknowledgePacket(std::shared_ptr<ConnectionReadyPacket> packet);

 private:
  PeerSession& session_;

  UniquePKey pub_key_ = nullptr;
  UniquePKey peer_pkey_ = nullptr;
  bool sent_handshake_ = false;
  bool sent_ready_ = false;
  bool enable_encryption_ = false;
  bool want_encryption_ = true;  // server policy, unread on the initiator
  bool negotiated_ = false;      // mode settled; ready may now be exchanged
  unsigned char* shared_secret_ = nullptr;
  size_t shared_secret_len_ = 0;
  bool key_filled_ = false;
  unsigned char* key_ = nullptr;
  size_t key_len_ = 0;
  EVP_CIPHER_CTX* enc_ctx_ = nullptr;
  EVP_CIPHER_CTX* dec_ctx_ = nullptr;
  bool cipher_keyed_ = false;  // encrypt side
  bool dec_keyed_ = false;     // decrypt side, keyed on its first inbound message
  // SendPacket runs on the caller's thread and a session may be sent to from
  // several, so the shared encrypt context needs guarding. The decrypt side
  // does not: it is only ever touched by the worker owning the session.
  std::mutex enc_mutex_;

 private:
  std::shared_ptr<Buffer> HandleDecrypt(std::shared_ptr<Buffer> buffer);
  void SendHandshake();
  void SendReady();
};

}