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

#include <any>
#include "../buffer.h"
#include "packet.h"

namespace znet {

template <typename T,
          std::enable_if_t<std::is_base_of_v<Packet, T>, bool> = true>
class PacketSerializer {
 public:
  PacketSerializer(PacketId packet_id) : packet_id_(packet_id) {}

  virtual Ref<Buffer> Serialize(Ref<T> packet, Ref<Buffer> buffer) {
    return CreateRef<Buffer>();
  }

  virtual Ref<T> Deserialize(Ref<Buffer> buffer) { return CreateRef<T>(); }

  PacketId packet_id() const { return packet_id_; }

 private:
  PacketId packet_id_;
};

}  // namespace znet