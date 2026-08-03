//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// The game protocol: a login exchange, then chat. Only LoginPacket has anything
// to do with authentication.
//
// znet's key exchange is anonymous, so an intercepted connection completes
// normally and a token sent over it proves nothing: whoever relayed it can
// present it themselves and be accepted as its owner. So LoginPacket carries a
// signature over PeerSession::ExportKeyingMaterial as well as the token. Both
// ends of one session derive the same export, nobody else can, and every session
// derives a different one, so an interceptor holds two unrelated values and
// cannot make a proof from one satisfy the other.
//

#pragma once

#include "token.h"

#include "znet/codec.h"
#include "znet/packet.h"

using namespace znet;

// Ties an export to this use. Anything unique to your protocol works; the
// version lets the scheme change later without a silent mismatch.
static const char kAuthExportLabel[] = "znet-example-auth v1";
static constexpr size_t kExportLength = 32;

enum PacketType : PacketId { kPacketLogin = 1, kPacketLoginResult, kPacketChat };

class LoginPacket : public Packet {
 public:
  LoginPacket() : Packet(kPacketLogin) {}

  AuthToken token;
  // Ed25519 over this session's export, by the key the token names. This is the
  // part that does not survive being relayed onto another session.
  unsigned char proof[kEd25519SigLength] = {};
};

class LoginSerializer : public PacketSerializer<LoginPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<LoginPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->token.user_id);
    buffer->Write(packet->token.client_public_key, kEd25519KeyLength);
    buffer->WriteInt<int64_t>(packet->token.expires_at);
    buffer->Write(packet->token.service_signature, kEd25519SigLength);
    buffer->Write(packet->proof, kEd25519SigLength);
    return buffer;
  }

  std::shared_ptr<LoginPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<LoginPacket>();
    packet->token.user_id = buffer->ReadString();
    buffer->Read(packet->token.client_public_key, kEd25519KeyLength);
    packet->token.expires_at = buffer->ReadInt<int64_t>();
    buffer->Read(packet->token.service_signature, kEd25519SigLength);
    buffer->Read(packet->proof, kEd25519SigLength);
    return packet;
  }
};

class LoginResultPacket : public Packet {
 public:
  LoginResultPacket() : Packet(kPacketLoginResult) {}

  bool accepted = false;
  std::string detail;
};

class LoginResultSerializer : public PacketSerializer<LoginResultPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<LoginResultPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteInt<uint8_t>(packet->accepted ? 1 : 0);
    buffer->WriteString(packet->detail);
    return buffer;
  }

  std::shared_ptr<LoginResultPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<LoginResultPacket>();
    packet->accepted = buffer->ReadInt<uint8_t>() != 0;
    packet->detail = buffer->ReadString();
    return packet;
  }
};

class ChatPacket : public Packet {
 public:
  ChatPacket() : Packet(kPacketChat) {}

  std::string text;
};

class ChatSerializer : public PacketSerializer<ChatPacket> {
 public:
  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<ChatPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    buffer->WriteString(packet->text);
    return buffer;
  }

  std::shared_ptr<ChatPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<ChatPacket>();
    packet->text = buffer->ReadString();
    return packet;
  }
};

inline std::shared_ptr<Codec> MakeAuthCodec() {
  auto codec = std::make_shared<Codec>();
  codec->Add(kPacketLogin, std::make_unique<LoginSerializer>());
  codec->Add(kPacketLoginResult, std::make_unique<LoginResultSerializer>());
  codec->Add(kPacketChat, std::make_unique<ChatSerializer>());
  return codec;
}
