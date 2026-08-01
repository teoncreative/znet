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
// FlatBuffers payloads in a znet Buffer.
//
//   #include "znet/ext/flatbuffers/flatbuffers.h"
//
// the whole point of this extension is one rule: a flatbuffer that came off
// the network must be verified before anything reads it. FlatBuffers is fast
// because accessors are raw pointer arithmetic over the payload, with no
// bounds checks at all, so a hostile buffer whose internal offsets point
// outside itself turns GetRoot() into an arbitrary read. flatbuffers ships a
// Verifier for exactly this, and it is easy to forget.
//
// so there is no way to get a root out of here without one having run.
//

#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/packet.h"
#include "znet/packet_serializer.h"

namespace znet {
namespace ext {

/** @brief Bounds applied to a flatbuffer arriving off the wire. */
struct FlatBufferLimits {
  /** @brief Largest payload accepted, checked before anything is allocated. */
  size_t max_bytes = static_cast<size_t>(1) << 20;  // 1 MiB

  /** @brief Deepest nesting of tables and vectors the verifier will follow. */
  uint32_t max_depth = 64;

  /** @brief Most tables the verifier will visit before giving up. */
  uint32_t max_tables = 1000000;
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/** @brief Writes @p size bytes of finished flatbuffer, varint length-prefixed. */
inline void WriteFlatBuffer(Buffer& buffer, const uint8_t* data, size_t size) {
  buffer.WriteVarInt(size);
  if (data != nullptr && size != 0) {
    buffer.Write(data, size);
  }
}

/** @brief Writes the finished contents of @p builder. */
inline void WriteFlatBuffer(Buffer& buffer,
                            const flatbuffers::FlatBufferBuilder& builder) {
  WriteFlatBuffer(buffer, builder.GetBufferPointer(),
                  static_cast<size_t>(builder.GetSize()));
}

/** @brief Writes a detached buffer. */
inline void WriteFlatBuffer(Buffer& buffer,
                            const flatbuffers::DetachedBuffer& detached) {
  WriteFlatBuffer(buffer, detached.data(), detached.size());
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

namespace detail {

/**
 * @brief Whether @p pointer is aligned enough to read a flatbuffer from.
 *
 * The verifier's own alignment checking is relative to the start of the
 * buffer, so it cannot notice that the start itself is misaligned. Every
 * real allocator returns something suitable here, which is exactly why this
 * would go unnoticed on the one that does not.
 */
inline bool SuitablyAligned(const void* pointer) {
  return (reinterpret_cast<uintptr_t>(pointer) %
          alignof(flatbuffers::largest_scalar_t)) == 0;
}

}  // namespace detail

/**
 * @brief Reads a length-prefixed flatbuffer and verifies it as root type @p T.
 *
 * @return false, leaving @p out untouched, when the payload is oversized,
 *         truncated, or does not verify.
 *
 * On success @p out owns the bytes. It has to own them: accessors point into
 * the payload, so it must outlive every read, and the Buffer it arrived in
 * will be recycled. The copy also buys alignment, which a payload sitting at
 * an arbitrary offset behind a varint does not have.
 */
template <typename T>
bool ReadFlatBuffer(Buffer& buffer, std::vector<uint8_t>& out,
                    const FlatBufferLimits& limits = FlatBufferLimits()) {
  const size_t size = buffer.ReadVarInt<size_t>();
  if (size == 0 || size > limits.max_bytes || size > buffer.readable_bytes()) {
    return false;
  }

  std::vector<uint8_t> bytes(size);
  buffer.Read(bytes.data(), size);

  if (!detail::SuitablyAligned(bytes.data())) {
    return false;
  }

  flatbuffers::Verifier::Options options;
  options.max_depth = limits.max_depth;
  options.max_tables = limits.max_tables;
  options.max_size = limits.max_bytes;
  flatbuffers::Verifier verifier(bytes.data(), bytes.size(), options);
  if (!verifier.VerifyBuffer<T>(nullptr)) {
    return false;
  }

  out = std::move(bytes);
  return true;
}

/**
 * @brief The root of a payload that ReadFlatBuffer has already verified.
 *
 * Only call this on bytes that came back from a successful ReadFlatBuffer.
 */
template <typename T>
const T* GetVerifiedRoot(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) {
    return nullptr;
  }
  return flatbuffers::GetRoot<T>(bytes.data());
}

// ---------------------------------------------------------------------------
// Packet plumbing
// ---------------------------------------------------------------------------

/**
 * @brief A packet carrying one flatbuffer with root type @p T.
 *
 * Sending: fill `bytes` from a finished FlatBufferBuilder. Receiving: the
 * serializer has verified the payload, so Get() is safe to dereference.
 */
template <typename T>
class FlatBufferPacket : public Packet {
 public:
  explicit FlatBufferPacket(PacketId id) : Packet(id) {}

  /** @brief Takes ownership of a finished builder's bytes. */
  void SetFrom(const flatbuffers::FlatBufferBuilder& builder) {
    const uint8_t* data = builder.GetBufferPointer();
    bytes.assign(data, data + builder.GetSize());
  }

  /** @brief The root table, or nullptr if the packet is empty. */
  ZNET_NODISCARD const T* Get() const { return GetVerifiedRoot<T>(bytes); }

  std::vector<uint8_t> bytes;
};

/**
 * @brief Drops FlatBufferPacket<T> into znet's codec, verifying on the way in.
 *
 * @code
 *   codec->Add(kPacketState,
 *              std::make_unique<znet::ext::FlatBufferSerializer<State>>(kPacketState));
 * @endcode
 *
 * A payload that fails verification returns nullptr, which is how a
 * PacketSerializer drops a packet, so a malformed flatbuffer costs the packet
 * rather than the process.
 */
template <typename T>
class FlatBufferSerializer : public PacketSerializer<FlatBufferPacket<T>> {
 public:
  explicit FlatBufferSerializer(PacketId id,
                                FlatBufferLimits limits = FlatBufferLimits())
      : id_(id), limits_(limits) {}

  std::shared_ptr<Buffer> SerializeTyped(
      std::shared_ptr<FlatBufferPacket<T>> packet,
      std::shared_ptr<Buffer> buffer) override {
    if (!packet || packet->bytes.empty()) {
      return nullptr;
    }
    WriteFlatBuffer(*buffer, packet->bytes.data(), packet->bytes.size());
    return buffer;
  }

  std::shared_ptr<FlatBufferPacket<T>> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<FlatBufferPacket<T>>(id_);
    if (!ReadFlatBuffer<T>(*buffer, packet->bytes, limits_)) {
      return nullptr;
    }
    return packet;
  }

 private:
  PacketId id_;
  FlatBufferLimits limits_;
};

}  // namespace ext
}  // namespace znet
