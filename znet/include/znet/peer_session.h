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

#include "znet/precompiled.h"
#include "znet/encryption.h"
#include "znet/packet_handler.h"
#include "znet/transport.h"
#include "znet/codec.h"

namespace znet {

class PeerSession {
 public:
  PeerSession(std::shared_ptr<InetAddress> local_address,
              std::shared_ptr<InetAddress> remote_address,
              SocketHandle socket, bool is_initiator = false);
  PeerSession(const PeerSession&) = delete;
  PeerSession(PeerSession&&) = delete;

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

  void SetCodec(std::shared_ptr<Codec> codec) {
    codec_ = std::move(codec);
  }

  void SetHandler(std::shared_ptr<PacketHandlerBase> handler) {
    handler_ = std::move(handler);
  }

  template<typename T>
  void SetUserPointer(std::shared_ptr<T> ptr) {
    user_ptr_ = std::move(ptr);
  }

  template<typename T>
  ZNET_NODISCARD std::shared_ptr<T> user_ptr_typed() const {
    // Attempt to static_pointer_cast to the desired type T
    // This assumes the user knows the correct type.
    return std::static_pointer_cast<T>(user_ptr_);
  }

  template<typename Rep, typename Period>
  void SetExpiry(std::chrono::duration<Rep,Period> ttl) {
    expire_at_ = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(ttl);
  }

  ZNET_NODISCARD std::chrono::steady_clock::time_point connect_time() {
    return connect_time_;
  }

  ZNET_NODISCARD std::chrono::steady_clock::duration time_since_connect() const noexcept {
    return std::chrono::steady_clock::now() - connect_time_;
  }

  ZNET_NODISCARD long seconds_since_connect() const noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               time_since_connect()).count();
  }

 protected:
  friend class EncryptionLayer;

  virtual void Ready() {
    is_ready_ = true;
    connect_time_ = std::chrono::steady_clock::now();
  }

  bool IsExpired() const {
    if (!has_expiry_) {
      return false;
    }
    return std::chrono::steady_clock::now() > expire_at_;
  }

  SessionId session_id_;
  SocketHandle socket_;
  std::shared_ptr<InetAddress> local_address_;
  PortNumber local_port_;
  std::shared_ptr<InetAddress> remote_address_;
  PortNumber remote_port_;

  std::shared_ptr<Codec> codec_;
  std::shared_ptr<PacketHandlerBase> handler_;
  TransportLayer transport_layer_;
  EncryptionLayer encryption_layer_;
  bool is_initiator_;
  bool is_ready_ = false;
  bool is_alive_ = true;
  std::chrono::steady_clock::time_point connect_time_;
  std::chrono::steady_clock::time_point expire_at_;
  bool has_expiry_;
  std::shared_ptr<void> user_ptr_;
};
}  // namespace znet