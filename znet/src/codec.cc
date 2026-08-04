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

namespace {

// Enough to identify the stream and see the corruption without letting a peer
// fill the log through it.
constexpr size_t kDumpMaxBytes = 512;

void DumpUndecodableBuffer(Buffer& buffer, size_t frame_start) {
  const size_t total = buffer.size();
  const size_t shown = total < kDumpMaxBytes ? total : kDumpMaxBytes;
  const char* digits = "0123456789abcdef";
  const char* data = buffer.data();
  std::string hex;
  hex.reserve(shown * 3);
  for (size_t i = 0; i < shown; i++) {
    const unsigned char byte = static_cast<unsigned char>(data[i]);
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0x0F]);
    hex.push_back((i + 1) % 16 == 0 ? '\n' : ' ');
  }
  ZNET_LOG_WARN("Undecodable frame at offset {}, buffer ({} of {} bytes):\n{}",
                frame_start, shown, total, hex);
}

}  // namespace

DecodeStats Codec::Deserialize(std::shared_ptr<Buffer> buffer,
                               PacketHandlerBase& handler,
                               bool dump_on_failure) {
  DecodeStats stats;
  // the first failure dumps, once per buffer: every later frame is located by
  // the same framing that failure already put in doubt
  auto note_invalid = [&](size_t frame_start) {
    stats.invalid_frames++;
    if (dump_on_failure && stats.invalid_frames == 1) {
      DumpUndecodableBuffer(*buffer, frame_start);
    }
  };
  while (buffer->readable_bytes() > 0) {
    const size_t frame_start = buffer->read_cursor();
    auto packet_id = buffer->ReadVarInt<PacketId>();
    const size_t size = buffer->ReadInt<uint32_t>();
    BufferError error = buffer->GetAndClearLastError();
    if (error != BufferError::None) {
      ZNET_LOG_WARN("Reading packet header failed, dropping buffer!");
      note_invalid(frame_start);
      stats.framing_lost = true;
      break;
    }
    if (size > buffer->readable_bytes()) {
      // a declared length no buffer could back is the same untrustworthy
      // framing as an over-read; nothing after it can be located
      ZNET_LOG_WARN("Packet {} declares {} bytes with {} left, dropping buffer!",
                    packet_id, size, buffer->readable_bytes());
      note_invalid(frame_start);
      stats.framing_lost = true;
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
      note_invalid(frame_start);
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
      note_invalid(frame_start);
      // overrunning the read limit means the framing is no longer trustworthy,
      // so nothing after this point can be located. no rewind: the buffer goes.
      stats.framing_lost = true;
      break;
    }
    buffer->SetReadLimit(0);
    handler.Handle(pk);
  }
  return stats;
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
  // four bytes, not size_t: a frame is bounded far below 4 GiB and the old
  // eight-byte field was pure overhead on every message
  buffer->WriteInt<uint32_t>(0);  // length placeholder, backfilled below
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
  buffer->set_write_cursor(write_cursor - sizeof(uint32_t));
  buffer->WriteInt(static_cast<uint32_t>(size));
  buffer->set_write_cursor(write_cursor_end);
  return buffer;
}

void Codec::Add(PacketId id, std::unique_ptr<PacketSerializerBase> serializer) {
  serializers_.insert(std::pair<PacketId, std::unique_ptr<PacketSerializerBase>>(id, std::move(serializer)));
}

}
