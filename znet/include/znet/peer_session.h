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

#include "znet/task.h"
#include "znet/codec.h"
#include "znet/compression.h"
#include "znet/encryption.h"
#include "znet/packet_handler.h"
#include "znet/precompiled.h"
#include "znet/send_options.h"
#include "znet/transport.h"

namespace znet {

/**
 * @class PeerSession
 * @brief Represents a network session between a local and remote peer.
 *
 * PeerSession handles communication between two network peers, managing the
 * transport layer, encryption, and packet handling. It also supports session
 * expiration and user-defined data attachment.
 *
 * The class does not allow copy or move semantics to ensure each session
 * instance is unique.
 */
class PeerSession {
 public:
  PeerSession(std::shared_ptr<InetAddress> local_address,
              std::shared_ptr<InetAddress> remote_address,
              std::unique_ptr<TransportLayer> transport_layer, bool is_initiator = false,
              bool self_managed = false);
  PeerSession(const PeerSession&) = delete;
  PeerSession(PeerSession&&) = delete;
  ~PeerSession();

  void Process();

  Result Close(CloseOptions options = {});

  bool IsAlive();

  bool IsReady() { return is_ready_; }

  ZNET_NODISCARD SessionId id() const {
    return id_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const {
    return local_address_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> remote_address() const {
    return remote_address_;
  }

  bool SendPacket(std::shared_ptr<Packet> packet, SendOptions options = {});

  bool SendRaw(std::shared_ptr<Buffer> buffer, SendOptions options = {});

  void SetCodec(std::shared_ptr<Codec> codec) {
    codec_ = std::move(codec);
  }

  void SetHandler(std::shared_ptr<PacketHandlerBase> handler) {
    handler_ = std::move(handler);
  }

  /**
   * @brief Associates user-defined data with the PeerSession.
   *
   * Allows attaching a user-defined object to the session for custom purposes.
   * The object is held using a shared pointer and replaces any previously set data.
   *
   * @tparam T The type of the user-defined object.
   * @param ptr Shared pointer to the object to associate with the session.
   */
  template<typename T>
  void SetUserPointer(std::shared_ptr<T> ptr) {
    user_ptr_ = std::move(ptr);
  }

  /**
   * @brief Retrieves the user-defined object associated with the session, cast to the specified type.
   *
   * The caller should ensure the requested type matches the actual type of the stored object, otherwise it will throw an error.
   *
   * @tparam T The desired type of the user-defined object.
   * @return std::shared_ptr<T> Pointer to the user-defined object cast to type T.
   */
  template<typename T>
  ZNET_NODISCARD std::shared_ptr<T> user_ptr_typed() const {
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

  CompressionType out_compression_type() const {
    return out_compression_type_;
  }

  void SetOutCompression(CompressionType type) {
    out_compression_type_ = type;
    ZNET_LOG_INFO("Set out compression to {} for {}", GetCompressionTypeName(type), id_);
  }

 protected:
  friend class EncryptionLayer;

  void Ready();

  bool IsExpired() const {
    if (!has_expiry_) {
      return false;
    }
    return std::chrono::steady_clock::now() > expire_at_;
  }

  SessionId id_;
  std::shared_ptr<InetAddress> local_address_;
  PortNumber local_port_;
  std::shared_ptr<InetAddress> remote_address_;
  PortNumber remote_port_;

  std::shared_ptr<Codec> codec_;
  std::shared_ptr<PacketHandlerBase> handler_;
  std::unique_ptr<TransportLayer> transport_layer_;
  EncryptionLayer encryption_layer_;
  CompressionType out_compression_type_ = CompressionType::None;
  bool is_initiator_;
  bool is_ready_ = false;
  std::chrono::steady_clock::time_point connect_time_;
  std::chrono::steady_clock::time_point expire_at_;
  bool has_expiry_ = false;
  std::shared_ptr<void> user_ptr_;
  Task task_;
};
}  // namespace znet