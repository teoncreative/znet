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

#include "precompiled.h"
#include "encryption.h"
#include "packet_handler.h"
#include "transport.h"
#include "codec.h"

namespace znet {

class PeerSession {
 public:
  PeerSession(std::shared_ptr<InetAddress> local_address, std::shared_ptr<InetAddress> remote_address,
                    SocketHandle socket,
                     bool is_initiator = false);

  void Process();

  Result Close();

  bool IsAlive() { return is_alive_; }

  bool IsReady() { return is_ready_; }

  ZNET_NODISCARD SessionId session_id() const {
    return session_id_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const {
    return local_address_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> remote_address() const {
    return remote_address_;
  }

  ZNET_NODISCARD TransportLayer& transport_layer() { return transport_layer_; }

  bool SendPacket(std::shared_ptr<Packet> packet);

  void SetUserPointer(std::weak_ptr<void> ptr) {
    user_ptr_ = ptr;
  }

  void SetCodec(std::shared_ptr<Codec> codec) {
    codec_ = codec;
  }

  void SetHandler(std::shared_ptr<PacketHandlerBase> handler) {
    handler_ = handler;
  }

  template<typename T>
  void SetUserPointer(std::shared_ptr<T> ptr) {
    user_ptr_ = std::weak_ptr<void>(ptr);
  }

  ZNET_NODISCARD std::weak_ptr<void> user_ptr() const {
    return user_ptr_;
  }

  template<typename T>
  ZNET_NODISCARD std::shared_ptr<T> user_ptr_typed() const {
    // Lock the weak_ptr<void> to get a shared_ptr<void>
    std::shared_ptr<void> locked_ptr = user_ptr_.lock();

    if (locked_ptr) {
      // Attempt to static_pointer_cast to the desired type T
      // This assumes the user knows the correct type.
      return std::static_pointer_cast<T>(locked_ptr);
    }
    // If the weak_ptr is expired, or no object was set, return nullptr
    return nullptr;
  }

 protected:
  friend class EncryptionLayer;

  virtual void Ready() {
    is_ready_ = true;
  }

  SessionId session_id_;
  SocketHandle socket_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<InetAddress> remote_address_;

  TransportLayer transport_layer_;
  EncryptionLayer encryption_layer_;
  std::shared_ptr<Codec> codec_;
  std::shared_ptr<PacketHandlerBase> handler_;
  bool is_initiator_;
  bool is_ready_ = false;
  bool is_alive_ = false;
  std::weak_ptr<void> user_ptr_;
};
}  // namespace znet