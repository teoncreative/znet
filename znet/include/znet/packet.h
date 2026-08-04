//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PACKET_H_
#define ZNET_PACKET_H_

#include "znet/precompiled.h"

namespace znet {

using PacketId = uint64_t;

/**
 * @brief Base of every application message.
 *
 * Derive, add the fields, give the class a unique PacketId, and register a
 * PacketSerializer for it with Codec::Add(). The id is what travels on the
 * wire, so it has to be stable across builds and identical on both ends.
 */
class Packet {
 public:
  explicit Packet(PacketId id) : id_(id) {}
  virtual ~Packet() = default;

  ZNET_NODISCARD PacketId id() const { return id_; }

 private:
  PacketId id_;
};
}  // namespace znet

#endif  // ZNET_PACKET_H_
