//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "packet.h"
#include "packet_serializer.h"
#include "znet/logger.h"

namespace znet {

class ConnectionSession;

template <typename PacketType,
          std::enable_if_t<std::is_base_of_v<Packet, PacketType>, bool> = true>
using PacketHandlerFn =
    std::function<void(ConnectionSession&, Ref<PacketType>)>;

class PacketHandlerBase {
 public:
  virtual void Handle(ConnectionSession& session, Ref<Buffer> buffer) {}

  virtual Ref<Buffer> Serialize(ConnectionSession& session,
                                Ref<Packet> packet) {
    return nullptr;
  }

  virtual PacketId packet_id() { return 0; };

 private:
};

template <typename PacketType, typename PacketSerializerType,
          std::enable_if_t<std::is_base_of_v<PacketSerializer<PacketType>,
                                             PacketSerializerType>,
                           bool> = true>
class PacketHandler : public PacketHandlerBase {
 public:
  // Default constructor
  PacketHandler() { serializer_ = CreateRef<PacketSerializerType>(); }

  explicit PacketHandler(Ref<PacketSerializerType> serializer) {
    serializer_ = serializer;
  }

  void AddReceiveCallback(PacketHandlerFn<PacketType> fn) {
    callbacks_.push_back(fn);
  }

  void Handle(ConnectionSession& session, Ref<Buffer> buffer) override {
    auto packet = serializer_->Deserialize(buffer);
    for (const auto& item : callbacks_) {
      item(session, packet);
    }
  }

  Ref<Buffer> Serialize(ConnectionSession& session,
                        Ref<Packet> packet) override {
    Ref<Buffer> buffer = CreateRef<Buffer>();
    buffer->WriteInt(packet_id());
    buffer->WriteInt<size_t>(0);
    size_t write_cursor = buffer->write_cursor();
    auto ptr = buffer.get();
    buffer = serializer_->Serialize(
        std::static_pointer_cast<PacketType>(packet), buffer);
    // Serializer can change the buffer, so we need to check if it changed.
    if (ptr == buffer.get()) {
      size_t write_cursor_end = buffer->write_cursor();
      size_t size = write_cursor_end - write_cursor;
      buffer->SetWriteCursor(write_cursor - sizeof(size_t));
      buffer->WriteInt(size);
      buffer->SetWriteCursor(write_cursor_end);
    }
    return buffer;
  }

  PacketId packet_id() override { return serializer_->packet_id(); }

 private:
  Ref<PacketSerializerType> serializer_;
  std::vector<PacketHandlerFn<PacketType>> callbacks_;
};

class HandlerLayer {
 public:
  HandlerLayer() = default;
  ~HandlerLayer() = default;

  void Handle(ConnectionSession& session, Ref<Buffer> buffer) {
    while (buffer->ReadableBytes()) {
      auto packet_id = buffer->ReadInt<PacketId>();
      auto size = buffer->ReadInt<size_t>();
      if (buffer->IsFailedToRead()) {
        ZNET_LOG_DEBUG("Reading packet header failed, dropping buffer!");
        break;
      }
      size_t read_cursor = buffer->read_cursor();
      bool handled = false;
      for (const auto& item : handlers_) {
        if (packet_id == item.first) {
          item.second->Handle(session, buffer);
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

  Ref<Buffer> Serialize(ConnectionSession& session, Ref<Packet> packet) {
    for (const auto& item : handlers_) {
      if (packet->id() == item.first) {
        Ref<Buffer> buffer = item.second->Serialize(session, packet);
        if (buffer) {
          return buffer;
        }
      }
    }

    return nullptr;
  }

  void AddPacketHandler(Ref<PacketHandlerBase> handler) {
    handlers_[handler->packet_id()] = handler;
  }

 private:
  std::unordered_map<PacketId, Ref<PacketHandlerBase>> handlers_;
};
}  // namespace znet