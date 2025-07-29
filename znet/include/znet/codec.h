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
#include "znet/base/packet.h"
#include "znet/buffer.h"

namespace znet {

class Codec {
 public:
  Codec() {

  }

  void Deserialize(std::shared_ptr<Buffer> buffer, PacketHandlerBase& handler) {
    while (buffer->readable_bytes() > 0) {
      auto packet_id = buffer->ReadVarInt<PacketId>();
      auto size = buffer->ReadInt<size_t>();
      if (buffer->IsFailedToRead()) {
        ZNET_LOG_DEBUG("Reading packet header failed, dropping buffer!");
        break;
      }
      // Todo limit max read buffer
      size_t read_cursor = buffer->read_cursor();
      auto it = serializers_.find(packet_id);
      if (it == serializers_.end()) {
        ZNET_LOG_WARN("Serializer for packet {} does not exist!", packet_id);
        buffer->SkipRead(size);
        continue;
      }
      PacketSerializerBase& serializer = *it->second;
      std::shared_ptr<Packet> pk = serializer.Deserialize(buffer);
      if (!pk) {
        ZNET_LOG_WARN("Packet {} was not deserialized!", packet_id);
        buffer->SkipRead(size); // todo instead of skipping fixed amount, jump to the end
        continue;
      }
      handler.Handle(pk);
      size_t read_cursor_end = buffer->read_cursor();
      size_t read_bytes = read_cursor_end - read_cursor;
      if (read_bytes < size) {
        ZNET_LOG_WARN("Packet {} size mismatch! Expected {}, read {}",
                      packet_id, size, read_bytes);
      }
    }
  }

  std::shared_ptr<Buffer> Serialize(std::shared_ptr<Packet> packet) {
    auto it = serializers_.find(packet->id());
    if (it == serializers_.end()) {
      ZNET_LOG_WARN("Failed to find a serializer for packet {}!", packet->id());
      return nullptr;
    }
    PacketSerializerBase& serializer = *it->second;
    std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>();
    buffer->WriteVarInt(packet->id());
    buffer->WriteInt<size_t>(0);
    size_t write_cursor = buffer->write_cursor();
    auto ptr = buffer.get();
    buffer = serializer.Serialize(packet, buffer);
    // Serializer can change the buffer, so we need to check if it is changed.
    if (ptr == buffer.get()) {
      size_t write_cursor_end = buffer->write_cursor();
      size_t size = write_cursor_end - write_cursor;
      buffer->set_write_cursor(write_cursor - sizeof(size_t));
      buffer->WriteInt(size);
      buffer->set_write_cursor(write_cursor_end);
    }
    return buffer;
  }

  void Add(PacketId id, std::unique_ptr<PacketSerializerBase> serializer) {
    serializers_.insert(std::pair<PacketId, std::unique_ptr<PacketSerializerBase>>(id, std::move(serializer)));
  }
 private:
  std::unordered_map<PacketId, std::unique_ptr<PacketSerializerBase>> serializers_;
};

}


#endif  //ZNET_PARENT_CODEC_H
