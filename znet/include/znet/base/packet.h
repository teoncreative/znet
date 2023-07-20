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

#include "types.h"
#include "inet_addr.h"

namespace znet {

  class Packet {
  public:
    Packet(PacketId id) : id_(id) { }
    virtual ~Packet() { };

    PacketId id() const { return id_; }
  private:
    PacketId id_;
  };
}