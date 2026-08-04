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
#include "znet/message_pipeline.h"
#include "znet/outbound_queue.h"
#include "znet/options.h"
#include "znet/packet_handler.h"
#include "znet/precompiled.h"
#include "znet/send_options.h"
#include "znet/transport.h"

#include <vector>

namespace znet {

/**
 * @class PeerSession
 * @brief Represents a network session between a local and remote peer.
 *
 * PeerSession handles communication between two network peers, managing the
 * transport layer, encryption, and packet handling. It also supports session
 * expiration and user-defined data attachment.
 *
 * @par Threading
 * SendPacket() may be called from any thread and only queues. The codec, the
 * handler and the compression and encryption state belong to the worker
 * driving Process(), which is where queued packets are encoded, so SetCodec(),
 * SetHandler() and SetOutCompression() belong in an event or packet handler.
 *
 * The class does not allow copy or move semantics to ensure each session
 * instance is unique.
 */
class PeerSession {
 public:
  PeerSession(std::shared_ptr<InetAddress> local_address,
              std::shared_ptr<InetAddress> remote_address,
              std::unique_ptr<TransportLayer> transport_layer,
              ConnectionType connection_type,
              bool is_initiator = false,
              bool self_managed = false,
              const SessionOptions& options = {});
  PeerSession(const PeerSession&) = delete;
  PeerSession(PeerSession&&) = delete;
  ~PeerSession();

  /**
   * @brief One pass: transport upkeep, receive, dispatch, drain.
   *
   * @return true when the pass did something, i.e. a message arrived or the
   *         outbound queue drained. A self-managed session's loop naps after
   *         an idle pass instead of spinning a core.
   */
  bool Process();

  Result Close(CloseOptions options = {});

  bool IsAlive();

  /**
   * @brief Whether the handshake has settled and the session may be sent to.
   *
   * Acquire-loaded, so a thread that sees true also sees the codec, the
   * negotiated compression and the session keys the worker published before it.
   */
  bool IsReady() const { return is_ready_.load(std::memory_order_acquire); }

  /** @brief Starts from 1 and increments for each peer constructed. */
  ZNET_NODISCARD SessionId id() const {
    return id_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> local_address() const {
    return local_address_;
  }

  ZNET_NODISCARD std::shared_ptr<InetAddress> remote_address() const {
    return remote_address_;
  }

  /**
   * @brief Queues a packet for this session. Callable from any thread.
   *
   * Queues and returns; the worker encodes and sends within the same tick.
   * Nothing here locks or encodes, so a caller is never held up by a worker.
   * Fire and forget: a packet that fails to encode is logged and dropped.
   *
   * @param packet The packet to send.
   * @param options Per-message delivery options. Ignored by TCP.
   * @return false when the session is not ready, is closed, or already holds
   *         CommonOptions::send_queue_capacity packets. Refusal is the
   *         backpressure signal, and happens before encoding.
   */
  bool SendPacket(std::shared_ptr<Packet> packet, SendOptions options = {});

  /**
   * @brief Encodes and sends whatever SendPacket() has queued.
   *
   * Any thread may call this. Exactly one encodes a session at a time, so
   * ordering holds whichever one drains.
   *
   * @return whether anything was encoded, so a caller can skip waking the
   *         thread that flushes when there was nothing to flush.
   */
  bool DrainOutbound();

  /**
   * @brief Declares that a thread other than the worker drains this session.
   *
   * Set by a client that runs an encoder alongside its loop. Without it both
   * threads race for the encode claim and whichever wins first fixes the
   * connection's throughput for its lifetime; with it the worker only encodes
   * shallow queues, leaving anything deeper to overlap with the flush.
   */
  void SetHasDedicatedEncoder(bool has_encoder) {
    outbound_.SetHasDedicatedEncoder(has_encoder);
  }

  void SetCodec(std::shared_ptr<Codec> codec) {
    pipeline_.SetCodec(std::move(codec));
  }

  void SetHandler(std::shared_ptr<PacketHandlerBase> handler) {
    handler_ = std::move(handler);
  }

  /**
   * @brief Drops the packet handler.
   *
   * A handler almost always needs its session to reply with, and the obvious
   * way to arrange that is to hand it a shared_ptr. That closes a cycle: the
   * session owns the handler and the handler owns the session, so the refcount
   * on each never reaches zero. Neither is ever destroyed, and everything they
   * hold goes with them: the outbound queue, the transport, its send window,
   * the derived keys. Nothing on either side can detect it, and the leak scales
   * with connections served rather than showing up once.
   *
   * So whoever owns a session releases its handler when finished with it, which
   * for a server is when the session leaves its map. Call it from the thread
   * that drives Process(), since that is the thread dispatching into the
   * handler; the server does this from its worker.
   */
  void ReleaseHandler() { handler_.reset(); }

  /**
   * @brief Registers a callback fired when an idle session is sent to, so the
   *        owning worker encodes without waiting out its tick.
   *
   * Set by the server before it hands the session to the application, and never
   * replaced, so SendPacket() can read it without synchronizing. A session
   * driven directly, as a client's is, needs none.
   */
  void SetWakeCallback(std::function<void()> wake) {
    outbound_.SetWakeCallback(std::move(wake));
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
   *
   * @tparam T The desired type of the user-defined object.
   * @return std::shared_ptr<T> Pointer to the user-defined object cast to type T. This cast is unchecked, it is simply undefined behavior if the underlying type does not match the requested T. Returns null only if the pointer was not set. This is done for performance reasons.
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

  ZNET_NODISCARD uint64_t seconds_since_connect() const noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            time_since_connect()).count());
  }

  ZNET_NODISCARD CompressionType out_compression_type() const {
    return pipeline_.out_compression();
  }

  void SetOutCompression(CompressionType type) {
    pipeline_.SetOutCompression(type);
    ZNET_LOG_INFO("Set out compression to {} for {}", GetCompressionTypeName(type), id_);
  }

  ZNET_NODISCARD bool is_initiator() const {
    return is_initiator_;
  }

  ZNET_NODISCARD ConnectionType connection_type() const {
    return connection_type_;
  }

  ZNET_NODISCARD const SessionOptions& options() const {
    return options_;
  }

  /**
   * @brief Inbound frames from this peer that failed to decode.
   *
   * What CommonOptions::max_invalid_frames is judged against. Counted whether
   * or not metrics are compiled in.
   */
  ZNET_NODISCARD uint64_t invalid_frames() const { return invalid_frames_; }

  /**
   * @brief Derives bytes unique to this session, for binding an application
   *        credential to it.
   *
   * Both ends derive the same bytes for the same @p label, and no third party
   * can: the value comes from the key exchange, over a transcript of both public
   * keys. Every session gets different bytes, including two sessions to the same
   * peer.
   *
   * That is what it is for. znet's exchange is unauthenticated, so an intercepted
   * connection completes normally and a bearer token proves nothing: whoever
   * captured it can replay it onward and be accepted as its owner. An interceptor
   * runs two separate exchanges, so it cannot make one exported value serve both,
   * and a credential covering this value is worthless on any other session.
   *
   * A sketch of the exchange this is meant for: an authentication service issues
   * the client a token naming a public key; the client signs its export with the
   * matching private key; the server checks the token against the service's
   * public key and the signature against its own export of the same label. The
   * token, the service and the signature scheme are yours. This is only the part
   * that has to come from inside the session.
   *
   * Do not send the value itself. It is a shared secret, and revealing it lets a
   * listener produce whatever proof was built on it.
   *
   * @param label Names what the bytes are for, so two uses of this on one
   *              session cannot collide. Anything unique to your protocol does;
   *              a version in it lets you change the scheme later. 1 to
   *              EncryptionLayer::kMaxExportLabelLength bytes.
   * @param out Filled on success, untouched on failure.
   * @param out_len Up to EncryptionLayer::kMaxExportLength. 32 is the usual ask.
   * @return false if the session is unencrypted or not yet ready, or if the
   *         label or length is out of range. Call it once connected.
   */
  ZNET_NODISCARD bool ExportKeyingMaterial(const std::string& label,
                                           unsigned char* out,
                                           size_t out_len) const {
    return encryption_layer_.ExportKeyingMaterial(label, out, out_len);
  }

  /**
   * @brief Returns a snapshot of this session's counters.
   *
   * Combines the session's own message counters with the transport's. Cheap but
   * not free (it copies the struct and queries the transport), so sample it on a
   * timer rather than per packet. Returns a zeroed struct when znet is built
   * with ZNET_ENABLE_METRICS=0.
   */
  ZNET_NODISCARD SessionMetrics metrics() const {
#if ZNET_ENABLE_METRICS
    SessionMetrics out = metrics_;
    out.transport = connection_type_;
    if (transport_layer_) {
      transport_layer_->FillMetrics(out);
    }
    out.common.outbound_queued = static_cast<uint32_t>(outbound_.size());
    return out;
#else
    return {};
#endif
  }

 protected:
  friend class EncryptionLayer;

  void Ready();

  /**
   * @brief Encodes and sends a packet on the spot, skipping the queue.
   *
   * For the handshake, from the worker only. SendReady() settles
   * `enable_encryption_` immediately before sending, and one Process() call
   * dispatches a whole batch, so a queued handshake packet could go out
   * encrypted under a key the peer has not derived yet.
   */
  bool SendImmediate(std::shared_ptr<Packet> packet, SendOptions options = {});

  // the compression both directions will use. On an accepting session this is
  // the configured option; on an initiating one it is whatever the server
  // announced in its ready packet.
  ZNET_NODISCARD CompressionType negotiated_compression() const {
    return negotiated_compression_;
  }
  void SetNegotiatedCompression(CompressionType type) {
    negotiated_compression_ = type;
  }

  bool IsExpired() const {
    if (!has_expiry_) {
      return false;
    }
    return std::chrono::steady_clock::now() > expire_at_;
  }

  // how many messages one Process() call will deliver before yielding, so a
  // session under load cannot monopolize the worker it shares with others.
  static constexpr uint32_t kMaxReceivesPerProcess = 256;



 private:
  /**
   * @brief The whole send pipeline: serialize, compress, encrypt, transport.
   *
   * Runs under the encode claim, which is what lets the codec, the compression
   * type and the cipher state stay unguarded.
   */
  bool EncodeAndSend(const std::shared_ptr<Packet>& packet, SendOptions options);

 protected:
  SessionId id_;
  std::shared_ptr<InetAddress> local_address_;
  PortNumber local_port_;
  std::shared_ptr<InetAddress> remote_address_;
  PortNumber remote_port_;
  ConnectionType connection_type_;

  std::shared_ptr<PacketHandlerBase> handler_;
  std::unique_ptr<TransportLayer> transport_layer_;
  // must stay above encryption_layer_, whose constructor installs the handshake
  // codec through the session and so needs the pipeline already built. the
  // reference it binds is not dereferenced until Encode/Decode.
  MessagePipeline pipeline_;
  EncryptionLayer encryption_layer_;
  SessionOptions options_;
  CompressionType negotiated_compression_ = CompressionType::None;
  bool is_initiator_;
  // published with release once the handshake settles, so a sender that reads
  // it sees the codec and keys the worker wrote beforehand. See IsReady().
  std::atomic_bool is_ready_{false};
  std::chrono::steady_clock::time_point connect_time_;
  std::chrono::steady_clock::time_point expire_at_;
  bool has_expiry_ = false;
  // touched only by the thread that drives this session, like metrics_, but
  // lives outside the metrics build flag: the close threshold depends on it
  uint64_t invalid_frames_ = 0;
  std::shared_ptr<void> user_ptr_;
  Task task_;

  // the thread boundary on the send path: the queue, the encode claim and the
  // rule for who takes it
  OutboundQueue outbound_;
#if ZNET_ENABLE_METRICS
  // touched only by the thread that drives this session
  SessionMetrics metrics_;
#endif
};
}  // namespace znet