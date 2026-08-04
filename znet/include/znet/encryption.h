//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Internal: a private member of PeerSession, public only because that member
// needs the complete type. Nothing here is meant to be called directly.
//
#ifndef ZNET_ENCRYPTION_H_
#define ZNET_ENCRYPTION_H_

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

/**
 * @brief Anti-replay for the AEAD nonce counters, IPsec style: the highest
 *        counter seen plus a bitmap of the 64 below it. Reordering inside that
 *        window is accepted; a repeat, or anything older, is refused.
 *
 * One instance per ordering domain (TransportLayer::OrderingDomain), because a
 * stalled stream falls arbitrarily far behind a flowing one and a shared window
 * would refuse it as too old. Widening cannot fix that; the gap is unbounded.
 *
 * Only feed it counters whose tag has already verified, or one forged message
 * with a huge counter locks out every real one behind it.
 */
class ReplayWindow {
 public:
  static constexpr uint64_t kWidth = 64;

  /** @brief True if @p counter has not been seen; records it when so. */
  bool Accept(uint64_t counter) {
    if (!seen_any_) {
      seen_any_ = true;
      highest_ = counter;
      window_ = 0;
      return true;
    }
    if (counter > highest_) {
      const uint64_t shift = counter - highest_;
      if (shift > kWidth) {
        window_ = 0;  // everything tracked falls out of range
      } else if (shift == kWidth) {
        window_ = uint64_t{1} << (kWidth - 1);  // only the old highest survives
      } else {
        // the old highest becomes the entry `shift` back
        window_ = (window_ << shift) | (uint64_t{1} << (shift - 1));
      }
      highest_ = counter;
      return true;
    }
    const uint64_t back = highest_ - counter;
    if (back == 0 || back > kWidth) {
      return false;  // the highest itself, or too old to judge
    }
    const uint64_t bit = uint64_t{1} << (back - 1);
    if (window_ & bit) {
      return false;
    }
    window_ |= bit;
    return true;
  }

 private:
  uint64_t highest_ = 0;
  uint64_t window_ = 0;
  bool seen_any_ = false;
};

class PeerSession;

class EncryptionLayer {
 public:
  /** @brief Longest export HKDF-SHA256 can produce. */
  static constexpr size_t kMaxExportLength = 255 * 32;
  /** @brief Longest application label an export may carry. */
  static constexpr size_t kMaxExportLabelLength = 255;

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
  /**
   * @brief Encrypts one outgoing message.
   *
   * @param stream  which of the transport's independently-ordered streams this
   *                message travels in. Selects the counter and goes into the
   *                nonce, so each stream has its own sequence and its own
   *                replay window on the far side. A single-stream transport
   *                always passes 0.
   */
  std::shared_ptr<Buffer> HandleOut(std::shared_ptr<Buffer> buffer,
                                    uint8_t stream);

  void OnHandshakePacket(std::shared_ptr<HandshakePacket> packet);
  void OnAcknowledgePacket(std::shared_ptr<ConnectionReadyPacket> packet);

  /** @brief Bytes unique to this session and this label. See PeerSession. */
  Result ExportKeyingMaterial(const std::string& label, unsigned char* out,
                            size_t out_len) const;

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

  // AES-256-GCM, one key per direction. The two directions must never share a
  // key: the nonce is a counter, so a shared key would repeat a (key, nonce)
  // pair as soon as both sides sent, and repeating one under GCM leaks the
  // authentication subkey rather than just a block of plaintext. Which label
  // maps to tx and which to rx is decided by is_initiator().
  unsigned char tx_key_[32] = {};
  unsigned char rx_key_[32] = {};
  // Nonce is salt || counter. The salt half is derived, never sent, and only
  // separates the directions further; the counter half is what goes on the wire.
  unsigned char tx_salt_[4] = {};
  unsigned char rx_salt_[4] = {};

  // Root for ExportKeyingMaterial, derived once from the shared secret over a
  // transcript of both public keys. Independent of the keys above: same secret,
  // different HKDF info.
  unsigned char exporter_secret_[32] = {};

  // Indexed by ordering domain and grown on demand, so a session on a
  // single-stream transport carries one entry rather than all 256 the wire
  // allows.
  std::vector<uint64_t> tx_counters_;   // guarded by enc_mutex_
  std::vector<ReplayWindow> rx_replay_;  // worker thread only

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
  bool DeriveDirectionalKeys();
  bool DeriveExporterSecret();
  /** @brief Send counter for `stream`. Call with enc_mutex_ held. */
  uint64_t& TxCounter(uint8_t stream);
  /** @brief Replay window for `stream`. Worker thread only. */
  ReplayWindow& RxWindow(uint8_t stream);
  void SendHandshake();
  void SendReady();
};

}

#endif  // ZNET_ENCRYPTION_H_
