//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PACKET_SERIALIZER_H_
#define ZNET_PACKET_SERIALIZER_H_

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/packet.h"

namespace znet {

class PacketSerializerBase {
 public:
  virtual ~PacketSerializerBase() = default;

  /**
   * @brief Turns @p packet into bytes.
   *
   * Normally write into @p buffer and return it: the codec has already put the
   * frame header there, and Buffer grows on write, so there is no size to
   * respect. A serializer that already holds the bytes may instead return a
   * buffer of its own, whose readable range the codec copies in behind the
   * header. Return nullptr to refuse, which drops the packet.
   */
  virtual std::shared_ptr<Buffer> Serialize(std::shared_ptr<Packet> packet, std::shared_ptr<Buffer> buffer) = 0;

  /** @brief Reads one packet from @p buffer, or nullptr if it is not valid. */
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

#endif  // ZNET_PACKET_SERIALIZER_H_
