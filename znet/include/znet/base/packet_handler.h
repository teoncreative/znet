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
    buffer->WriteInt(packet->id());
    return serializer_->Serialize(std::static_pointer_cast<PacketType>(packet),
                                  buffer);
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
      if (buffer->IsFailedToRead()) {
        ZNET_LOG_DEBUG("Read failed!");
        break;
      }
      bool handled = false;
      for (const auto& item : handlers_) {
        if (packet_id == item.first) {
          item.second->Handle(session, buffer);
          handled = true;
          break;
        }
      }
      // For now, if it doesn't handle, it discards all the packets inside this
      // data. We should store and use the length of each packet
      if (!handled) {
        ZNET_LOG_WARN("Received packet wasn't handled!");
        break;
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