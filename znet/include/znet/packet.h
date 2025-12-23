//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"

namespace znet {

using PacketId = uint64_t;

class Packet {
 public:
  explicit Packet(PacketId id) : id_(id) {}
  virtual ~Packet() = default;

  PacketId id() const { return id_; }

 private:
  PacketId id_;
};
}  // namespace znet