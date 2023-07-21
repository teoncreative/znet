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

#include "packet_handler.h"

namespace znet {
class ConnectionSession {
 public:
  ConnectionSession(Ref<InetAddress> local_address,
                    Ref<InetAddress> remote_address)
      : local_address_(std::move(local_address)),
        remote_address_(std::move(remote_address)) {}

  virtual void Process() {}

  virtual void Close() {}

  virtual bool IsAlive() { return false; }

  ZNET_NODISCARD virtual Ref<InetAddress> local_address() const {
    return local_address_;
  }

  ZNET_NODISCARD virtual Ref<InetAddress> remote_address() const {
    return remote_address_;
  }

  virtual void SendPacket(Ref<Packet> packet) {}

  virtual void SendRaw(Ref<Buffer> buffer) {}

  HandlerLayer& handler_layer() { return handler_layer_; }

 protected:
  HandlerLayer handler_layer_;
  Ref<InetAddress> local_address_;
  Ref<InetAddress> remote_address_;
};
}  // namespace znet