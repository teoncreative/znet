//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "packet_handler.h"

namespace znet {

void HandlerLayer::HandleIn(Ref<Buffer> buffer) {
  while (buffer->readable_bytes() > 0) {
    auto packet_id = buffer->ReadVarInt<PacketId>();
    auto size = buffer->ReadInt<size_t>();
    if (buffer->IsFailedToRead()) {
      ZNET_LOG_DEBUG("Reading packet header failed, dropping buffer!");
      break;
    }
    size_t read_cursor = buffer->read_cursor();
    bool handled = false;
    for (const auto& item : handlers_) {
      if (packet_id == item.first) {
        item.second->Handle(session_, buffer);
        handled = true;
        break;
      }
    }
    if (!handled) {
      ZNET_LOG_WARN("Packet {} was not handled!", packet_id);
      buffer->SkipRead(size);
    } else {
      size_t read_cursor_end = buffer->read_cursor();
      size_t read_bytes = read_cursor_end - read_cursor;
      if (read_bytes < size) {
        ZNET_LOG_WARN("Packet {} size mismatch! Expected {}, read {}",
                      packet_id, size, read_bytes);
      }
    }
  }
}

Ref<Buffer> HandlerLayer::HandleOut(Ref<Packet> packet) {
  for (const auto& item : handlers_) {
    if (packet->id() == item.first) {
      Ref<Buffer> buffer = item.second->Serialize(session_, packet);
      if (buffer) {
        return buffer;
      }
    }
  }

  return nullptr;
}

void HandlerLayer::AddPacketHandler(Ref<PacketHandlerBase> handler) {
  handlers_[handler->packet_id()] = handler;
}

}  // namespace znet