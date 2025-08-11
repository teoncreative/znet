//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/codec.h"

namespace znet {

Codec::Codec() {

}

void Codec::Deserialize(std::shared_ptr<Buffer> buffer, PacketHandlerBase& handler) {
  while (buffer->readable_bytes() > 0) {
    auto packet_id = buffer->ReadVarInt<PacketId>();
    auto size = buffer->ReadInt<size_t>();
    BufferError error = buffer->GetAndClearLastError();
    if (error != BufferError::None) {
      ZNET_LOG_DEBUG("Reading packet header failed, dropping buffer!");
      break;
    }
    size_t read_cursor = buffer->read_cursor();
    auto it = serializers_.find(packet_id);
    if (it == serializers_.end()) {
      ZNET_LOG_WARN("Serializer for packet {} does not exist!", packet_id);
      buffer->SkipRead(size);
      continue;
    }
    // Set a read limit so the serializer cannot accidentally read more
    // than it should
    buffer->SetReadLimit(read_cursor + size);
    PacketSerializerBase& serializer = *it->second;
    std::shared_ptr<Packet> pk = serializer.Deserialize(buffer);
    if (!pk) {
      ZNET_LOG_WARN("Packet {} was not deserialized!", packet_id);
      // We go back to the start, in case it was partially read
      buffer->set_read_cursor(read_cursor);
      // Then skip the number of bytes given
      buffer->SkipRead(size);
      // Disable the read limit
      buffer->SetReadLimit(0);
      continue;
    }
    size_t read_cursor_end = buffer->read_cursor();
    size_t read_bytes = read_cursor_end - read_cursor;
    if (read_bytes < size) {
      ZNET_LOG_WARN("Packet {} size mismatch! Expected {}, read {}.",
                    packet_id, size, read_bytes);
      buffer->set_read_cursor(read_cursor);
      buffer->SkipRead(size);
    } else if (read_bytes > size) {
      ZNET_LOG_WARN("Packet {} size mismatch! Expected {}, read {}. This will drop the packet and rest of the buffer.",
                    packet_id, size, read_bytes);
      // TODO: Perhaps add an option to dump the buffer when this happens. This might be malicious attempt.
      // Unlike the other mismatch, we don't need to reset the cursor or anything like that since the buffer will be dropped.
      break;
    }
    // Disable the read limit
    buffer->SetReadLimit(0);
    handler.Handle(pk);
  }
}

std::shared_ptr<Buffer> Codec::Serialize(std::shared_ptr<Packet> packet) {
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
    // We do this only when the buffer is not changed
    size_t write_cursor_end = buffer->write_cursor();
    size_t size = write_cursor_end - write_cursor;
    buffer->set_write_cursor(write_cursor - sizeof(size_t));
    buffer->WriteInt(size);
    buffer->set_write_cursor(write_cursor_end);
  }
  return buffer;
}

void Codec::Add(PacketId id, std::unique_ptr<PacketSerializerBase> serializer) {
  serializers_.insert(std::pair<PacketId, std::unique_ptr<PacketSerializerBase>>(id, std::move(serializer)));
}

}
