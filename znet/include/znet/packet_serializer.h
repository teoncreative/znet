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

#include "znet/packet.h"
#include "znet/buffer.h"
#include "znet/precompiled.h"

namespace znet {

class PacketSerializerBase {
 public:
  virtual ~PacketSerializerBase() = default;

  virtual std::shared_ptr<Buffer> Serialize(std::shared_ptr<Packet> packet, std::shared_ptr<Buffer> buffer) = 0;
  virtual std::shared_ptr<Packet> Deserialize(std::shared_ptr<Buffer> buffer) = 0;
};

template <typename T>
class PacketSerializer : public PacketSerializerBase {
  static_assert(std::is_base_of<Packet, T>::value, "T must derive from Packet");

 public:
  virtual std::shared_ptr<Buffer> SerializeTyped(std::shared_ptr<T> packet, std::shared_ptr<Buffer> buffer) = 0;
  virtual std::shared_ptr<T> DeserializeTyped(std::shared_ptr<Buffer> buffer) = 0;

  // override base class
  std::shared_ptr<Buffer> Serialize(std::shared_ptr<Packet> packet, std::shared_ptr<Buffer> buffer) override {
    return SerializeTyped(std::static_pointer_cast<T>(packet), buffer);
  }

  std::shared_ptr<Packet> Deserialize(std::shared_ptr<Buffer> buffer) override {
    return DeserializeTyped(buffer);
  }
};

}  // namespace znet