//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PARENT_CODEC_H
#define ZNET_PARENT_CODEC_H

#include "znet/precompiled.h"
#include "znet/packet.h"
#include "znet/buffer.h"
#include "znet/packet_handler.h"
#include "znet/packet_serializer.h"

namespace znet {

/**
 * @brief Provides serialization and deserialization of packets.
 */
class Codec {
 public:
  Codec();
  ~Codec() = default;

  /**
   * @brief Deserializes packets from a buffer and handles them using the provided handler.
   *
   * Processes a binary buffer by reading packet IDs, sizes, and payloads. Uses the appropriate
   * serializer based on the packet ID to deserialize the packet. Deserialized packets are
   * passed to the given packet handler. Logs warnings for issues such as missing serializers,
   * deserialization failures, or size mismatches.
   *
   * @param buffer A shared pointer to the buffer containing packet data.
   * @param handler Reference to the packet handler responsible for processing deserialized packets.
   */
  void Deserialize(std::shared_ptr<Buffer> buffer, PacketHandlerBase& handler);

  /**
   * @brief Serializes a packet into a binary buffer.
   *
   * This function retrieves the appropriate serializer for the packet
   * based on its ID, writes the packet ID, and serializes the packet
   * into a binary buffer. Ensures the serialized size is properly recorded.
   *
   * @param packet A shared pointer to the packet to be serialized.
   * @return A shared pointer to the resulting serialized buffer.
   *         Returns nullptr if no serializer is found for the packet.
   */
  std::shared_ptr<Buffer> Serialize(std::shared_ptr<Packet> packet);

  /**
   * @brief Registers a packet serializer for a specific packet type.
   *
   * @param id Unique identifier for the packet type.
   * @param serializer Serializer instance to handle the packet type.
   */
  void Add(PacketId id, std::unique_ptr<PacketSerializerBase> serializer);

 private:
  std::unordered_map<PacketId, std::unique_ptr<PacketSerializerBase>> serializers_;
};

}


#endif  //ZNET_PARENT_CODEC_H
