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

#include "encryption.h"
#include "packet_handler.h"
#include "transport.h"

namespace znet {
class PeerSession {
 public:
  PeerSession(Ref<InetAddress> local_address, Ref<InetAddress> remote_address,
                    SocketHandle socket,
                     bool is_initiator = false);

  void Process();

  Result Close();

  bool IsAlive() { return is_alive_; }

  bool IsReady() { return is_ready_; }

  ZNET_NODISCARD Ref<InetAddress> local_address() const {
    return local_address_;
  }

  ZNET_NODISCARD Ref<InetAddress> remote_address() const {
    return remote_address_;
  }

  ZNET_NODISCARD HandlerLayer& handler_layer() { return handler_layer_; }

  ZNET_NODISCARD TransportLayer& transport_layer() { return transport_layer_; }

  void AddPacketHandler(Ref<PacketHandlerBase> handler) {
    handler_layer_.AddPacketHandler(handler);
  }

  bool SendPacket(Ref<Packet> packet);

 protected:
  friend class EncryptionLayer;
  virtual void Ready() {
    is_ready_ = true;
  }

  SocketHandle socket_;
  Ref<InetAddress> local_address_;
  Ref<InetAddress> remote_address_;

  TransportLayer transport_layer_;
  EncryptionLayer encryption_layer_;
  HandlerLayer handler_layer_;
  bool is_initiator_;
  bool is_ready_ = false;
  bool is_alive_ = false;
};
}  // namespace znet