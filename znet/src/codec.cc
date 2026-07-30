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
      ZNET_LOG_WARN("Serializer for packet {} does not exist! (have {} serializers)", packet_id, serializers_.size());
      buffer->SkipRead(size);
      continue;
    }
    // fences the serializer inside its own frame, so a malformed one cannot
    // read into the next packet
    buffer->SetReadLimit(read_cursor + size);
    PacketSerializerBase& serializer = *it->second;
    std::shared_ptr<Packet> pk = serializer.Deserialize(buffer);
    if (!pk) {
      ZNET_LOG_WARN("Packet {} was not deserialized!", packet_id);
      // it may have read part of the frame, so rewind and skip the whole
      // declared length to land on the next one
      buffer->set_read_cursor(read_cursor);
      buffer->SkipRead(size);
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
      // overrunning the read limit means the framing is no longer trustworthy,
      // so nothing after this point can be located. no rewind: the buffer goes.
      // TODO: option to dump the buffer here, this can be an attack.
      break;
    }
    buffer->SetReadLimit(0);
    handler.Handle(pk);
  }
}

std::shared_ptr<Buffer> Codec::Serialize(std::shared_ptr<Packet> packet,
                                         size_t headroom) {
  auto it = serializers_.find(packet->id());
  if (it == serializers_.end()) {
    ZNET_LOG_WARN("Failed to find a serializer for packet {}!", packet->id());
    return nullptr;
  }
  PacketSerializerBase& serializer = *it->second;
  std::shared_ptr<Buffer> buffer = std::make_shared<Buffer>();
  if (headroom != 0) {
    buffer->ReserveHeadroom(headroom);
  }
  buffer->WriteVarInt(packet->id());
  buffer->WriteInt<size_t>(0);  // length placeholder, backfilled below
  const size_t write_cursor = buffer->write_cursor();
  const Buffer* framed = buffer.get();
  std::shared_ptr<Buffer> out = serializer.Serialize(packet, buffer);
  if (!out) {
    ZNET_LOG_WARN("Serializer for packet {} produced nothing, dropping packet!",
                  packet->id());
    return nullptr;
  }
  // a serializer holding the bytes already, a cached encoding or a payload it
  // is forwarding, can hand back its own buffer rather than write them through
  // a second time. the frame header is in ours, so copy the body in behind it.
  if (out.get() != framed) {
    buffer->Write(out->read_cursor_data(), out->readable_bytes());
  }
  const size_t write_cursor_end = buffer->write_cursor();
  const size_t size = write_cursor_end - write_cursor;
  buffer->set_write_cursor(write_cursor - sizeof(size_t));
  buffer->WriteInt(size);
  buffer->set_write_cursor(write_cursor_end);
  return buffer;
}

void Codec::Add(PacketId id, std::unique_ptr<PacketSerializerBase> serializer) {
  serializers_.insert(std::pair<PacketId, std::unique_ptr<PacketSerializerBase>>(id, std::move(serializer)));
}

}
