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
// ZDT (znet Datagram Transport). See znet/backends/zdt.h for the overview.
//

#include "znet/backends/zdt/zdt_wire.h"

#include "znet/error.h"
#include "znet/logger.h"
#include "znet/util.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace znet {
namespace backends {

// ---------------------------------------------------------------------------
// Wire header
// ---------------------------------------------------------------------------

void WriteZDTHeader(Buffer& buffer, const ZDTHeader& header) {
  buffer.WriteInt<uint8_t>(header.flags);
  buffer.WriteInt<uint16_t>(header.packet_seq);
  buffer.WriteInt<uint16_t>(header.ack);
  buffer.WriteInt<uint8_t>(header.block_count);
  for (uint8_t i = 0; i < header.block_count; i++) {
    buffer.WriteInt<uint8_t>(header.blocks[i].num_ack);
    buffer.WriteInt<uint8_t>(header.blocks[i].num_nack);
  }
}

bool ReadZDTHeader(Buffer& buffer, ZDTHeader& out_header) {
  if (buffer.readable_bytes() < kZDTHeaderSize) {
    return false;
  }
  out_header.flags = buffer.ReadInt<uint8_t>();
  if (!(out_header.flags & kFlagOnline)) {
    return false;  // offline (handshake) message, not an online datagram
  }
  out_header.packet_seq = buffer.ReadInt<uint16_t>();
  out_header.ack = buffer.ReadInt<uint16_t>();
  uint8_t count = buffer.ReadInt<uint8_t>();
  // a peer cannot make us read past the datagram, nor overrun the array
  if (count > kZDTMaxAckBlocks ||
      buffer.readable_bytes() < count * kZDTAckBlockSize) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    out_header.blocks[i].num_ack = buffer.ReadInt<uint8_t>();
    out_header.blocks[i].num_nack = buffer.ReadInt<uint8_t>();
  }
  out_header.block_count = count;
  return true;
}

void WriteZDTRecord(Buffer& buffer, const ZDTRecord& record) {
  buffer.WriteInt<uint8_t>(record.flags);
  buffer.WriteInt<uint8_t>(record.channel);
  buffer.WriteInt<uint16_t>(record.message_seq);
  if (record.flags & kRecFragment) {
    buffer.WriteInt<uint8_t>(record.frag_index);
    buffer.WriteInt<uint8_t>(record.frag_count);
  }
  buffer.WriteInt<uint16_t>(record.length);
}

bool ReadZDTRecord(Buffer& buffer, ZDTRecord& out_record) {
  if (buffer.readable_bytes() < kZDTRecordHeaderSize) {
    return false;
  }
  out_record.flags = buffer.ReadInt<uint8_t>();
  out_record.channel = buffer.ReadInt<uint8_t>();
  out_record.message_seq = buffer.ReadInt<uint16_t>();
  if (out_record.flags & kRecFragment) {
    if (buffer.readable_bytes() < 4) {  // frag pair plus length
      return false;
    }
    out_record.frag_index = buffer.ReadInt<uint8_t>();
    out_record.frag_count = buffer.ReadInt<uint8_t>();
  } else {
    out_record.frag_index = 0;
    out_record.frag_count = 1;
  }
  out_record.length = buffer.ReadInt<uint16_t>();
  // a truncated or lying length must not walk the reader off the datagram
  return buffer.readable_bytes() >= out_record.length;
}

void WriteOfflineHeader(Buffer& buffer, ZDTOfflineMsg id) {
  buffer.WriteInt<uint8_t>(static_cast<uint8_t>(id));
  buffer.Write(kZDTMagic.data(), kZDTMagic.size());
}

bool ReadOfflineHeader(Buffer& buffer, ZDTOfflineMsg& out_id) {
  if (buffer.readable_bytes() < 1 + kZDTMagic.size()) {
    return false;
  }
  uint8_t id = buffer.ReadInt<uint8_t>();
  if (id & kFlagOnline) {
    return false;  // online datagram, not an offline message
  }
  std::array<uint8_t, kZDTMagic.size()> magic{};
  buffer.Read(magic.data(), magic.size());
  if (magic != kZDTMagic) {
    return false;
  }
  out_id = static_cast<ZDTOfflineMsg>(id);
  return true;
}

// ---------------------------------------------------------------------------
// Return-routability cookie
// ---------------------------------------------------------------------------

ZDTCookie ComputeCookie(const uint8_t* secret, size_t secret_len,
                        const std::string& peer_readable, uint32_t epoch) {
  std::string message = peer_readable;
  message.push_back('|');
  for (int i = 0; i < 4; i++) {
    message.push_back(static_cast<char>((epoch >> (i * 8)) & 0xFFu));
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  HMAC(EVP_sha256(), secret, static_cast<int>(secret_len),
       reinterpret_cast<const unsigned char*>(message.data()), message.size(),
       digest, &digest_len);
  ZDTCookie cookie{};
  size_t copy = std::min<size_t>(cookie.size(), digest_len);
  std::memcpy(cookie.data(), digest, copy);
  return cookie;
}

bool ConstTimeEqual(const ZDTCookie& a, const ZDTCookie& b) {
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

uint64_t GenerateGuid() {
  uint64_t guid = 0;
  RAND_bytes(reinterpret_cast<unsigned char*>(&guid), sizeof(guid));
  return guid;
}

}  // namespace backends
}  // namespace znet
