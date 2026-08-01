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

#include <nlohmann/json.hpp>

#include <memory>

#include "znet/packet.h"
#include "znet/packet_serializer.h"
#include "znet/ext/json/codec.h"

namespace znet {
namespace ext {

/** @brief A packet whose whole body is one json document. */
class JsonPacket : public Packet {
 public:
  explicit JsonPacket(PacketId id) : Packet(id) {}

  nlohmann::json body;
};

/**
 * @brief Drops JsonPacket into znet's codec.
 *
 * @code
 *   codec->Add(kPacketConfig,
 *              std::make_unique<znet::ext::JsonSerializer>(kPacketConfig));
 * @endcode
 *
 * One serializer per packet id, since the id is what a decoded packet is
 * stamped with.
 *
 * Deserialize returns nullptr on anything malformed, which is how a
 * PacketSerializer says "drop this", so an oversized, over-nested or corrupt
 * document costs the packet and nothing more.
 */
class JsonSerializer : public PacketSerializer<JsonPacket> {
 public:
  explicit JsonSerializer(PacketId id, JsonLimits limits = JsonLimits())
      : id_(id), limits_(limits) {}

  /** @brief Whether the body travels as text instead of MessagePack. */
  void set_text(bool text) { text_ = text; }

  std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<JsonPacket> packet,
                                         std::shared_ptr<Buffer> buffer) override {
    if (!packet) {
      return nullptr;
    }
    if (text_) {
      WriteJsonText(*buffer, packet->body);
    } else {
      WriteJson(*buffer, packet->body);
    }
    return buffer;
  }

  std::shared_ptr<JsonPacket> DeserializeTyped(
      std::shared_ptr<Buffer> buffer) override {
    auto packet = std::make_shared<JsonPacket>(id_);
    const bool ok = text_ ? ReadJsonText(*buffer, packet->body, limits_)
                          : ReadJson(*buffer, packet->body, limits_);
    if (!ok) {
      return nullptr;
    }
    return packet;
  }

 private:
  PacketId id_;
  JsonLimits limits_;
  bool text_ = false;
};

}  // namespace ext
}  // namespace znet
