//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <memory>
#include <utility>

#include "znet/packet.h"
#include "znet/packet_serializer.h"
#include "znet/ext/reflect/serialize.h"

namespace znet {
namespace ext {

/**
 * @brief A packet whose body is one automatically serialized struct.
 *
 * @code
 *   struct Move { uint32_t entity; float x, y, z; };
 *
 *   using MovePacket = znet::ext::AutoPacket<Move>;
 *   codec->Add(kMove, znet::ext::MakeAutoSerializer<Move>(kMove));
 * @endcode
 *
 * No serializer to write and none to keep in step with the struct: adding a
 * field to Move changes both ends at once. That cuts the other way too, so a
 * field added on one side and not the other is a wire break with nothing to
 * catch it. Version the packet id when the struct changes.
 */
template <typename T>
class AutoPacket : public Packet {
 public:
  explicit AutoPacket(PacketId id) : Packet(id) {}
  AutoPacket(PacketId id, T initial) : Packet(id), body(std::move(initial)) {}

  T body{};
};

/** @brief Serializes AutoPacket<T> through the reflected field walk. */
template <typename T>
class AutoSerializer : public PacketSerializer<AutoPacket<T>> {
 public:
  explicit AutoSerializer(PacketId id, ReflectLimits limits = ReflectLimits())
      : id_(id), limits_(limits) {}

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<AutoPacket<T>> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    if (!packet) {
      return nullptr;
    }
    WriteAuto(*buffer, packet->body);
    return buffer;
  }

  std::shared_ptr<AutoPacket<T>> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<AutoPacket<T>>(id_);
    if (!ReadAuto(*buffer, packet->body, limits_)) {
      return nullptr;  // truncated or implausible; drop the packet
    }
    return packet;
  }

 private:
  PacketId id_;
  ReflectLimits limits_;
};

/** @brief Convenience for codec->Add(id, MakeAutoSerializer<T>(id)). */
template <typename T>
std::unique_ptr<AutoSerializer<T>> MakeAutoSerializer(
    PacketId id, ReflectLimits limits = ReflectLimits()) {
  return std::unique_ptr<AutoSerializer<T>>(new AutoSerializer<T>(id, limits));
}

}  // namespace ext
}  // namespace znet
