//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Internal: a private member of PeerSession, public only because that member
// needs the complete type. Nothing here is meant to be called directly.
//
// The transformation between packets and wire bytes, in one place and in both
// directions. It used to live in two: the outbound half inside the session's
// send function and the inbound half inline in its tick loop, which meant the
// two orderings had to be kept mirror images of each other by reading them side
// by side. They are each other's inverse, so they belong together.
//

#ifndef ZNET_MESSAGE_PIPELINE_H_
#define ZNET_MESSAGE_PIPELINE_H_

#include "znet/codec.h"
#include "znet/compat.h"
#include "znet/compression.h"
#include "znet/packet_handler.h"
#include "znet/types.h"

#include <memory>

namespace znet {

class EncryptionLayer;

/**
 * @brief Serializes, compresses and encrypts outgoing messages, and reverses
 *        that for incoming ones.
 *
 * @par Threading
 * Not synchronized. Everything here belongs to whichever thread currently holds
 * the session's encode claim, which is what lets the codec and the cipher state
 * be touched without a lock.
 */
class MessagePipeline {
 public:
  /**
   * @param encryption  owned by the session, not by this: it also drives the
   *                    handshake, so its lifetime is the session's.
   * @param id          only for log messages, so a line names its session.
   */
  MessagePipeline(EncryptionLayer& encryption, SessionId id)
      : encryption_(encryption), id_(id) {}

  MessagePipeline(const MessagePipeline&) = delete;
  MessagePipeline& operator=(const MessagePipeline&) = delete;

  /**
   * @brief Packet to wire bytes: serialize, compress, encrypt.
   *
   * Compression runs before encryption because ciphertext is incompressible,
   * so the other order costs a full pass and saves nothing.
   *
   * @param stream  which of the transport's independently-ordered streams this
   *                message travels in, from TransportLayer::OrderingDomain().
   *                The cipher keeps a sequence and a replay window per stream,
   *                since a sequence only means anything inside one.
   * @return null if any stage fails, having logged why.
   */
  std::shared_ptr<Buffer> Encode(const std::shared_ptr<Packet>& packet,
                                 uint8_t stream);

  /**
   * @brief Wire bytes to payload: decrypt, then decompress.
   *
   * The exact inverse of Encode's middle two stages, in the reverse order.
   *
   * @return null if either stage fails, having logged why.
   */
  std::shared_ptr<Buffer> Decode(std::shared_ptr<Buffer> buffer);

  /**
   * @brief Reads packets out of a decoded payload and hands them to `handler`.
   *
   * @return What failed to decode. The session counts these; the codec is
   *         shared and cannot.
   */
  DecodeStats Dispatch(const std::shared_ptr<Buffer>& payload,
                       PacketHandlerBase& handler);

  ZNET_NODISCARD bool has_codec() const { return codec_ != nullptr; }

  void SetCodec(std::shared_ptr<Codec> codec) { codec_ = std::move(codec); }

  ZNET_NODISCARD CompressionType out_compression() const {
    return out_compression_;
  }
  void SetOutCompression(CompressionType type) { out_compression_ = type; }

  /** @brief Messages below this many bytes skip compression entirely. */
  void SetCompressionThreshold(size_t bytes) { compression_threshold_ = bytes; }

  /** @brief Log a hex dump when a frame in a payload fails to decode. */
  void SetDumpOnDecodeFailure(bool enabled) {
    dump_on_decode_failure_ = enabled;
  }

  /**
   * @brief Bytes reserved in front of a serialized payload.
   *
   * The compression and encryption stages each prepend one byte, and the TCP
   * transport prepends its two-byte frame length. Giving every stage room to
   * work in place is worth several full copies of the payload per message.
   */
  static constexpr size_t kSendHeadroom = 4;

 private:
  EncryptionLayer& encryption_;
  SessionId id_;
  std::shared_ptr<Codec> codec_;
  CompressionType out_compression_ = CompressionType::None;
  size_t compression_threshold_ = 128;
  bool dump_on_decode_failure_ = false;
};

}  // namespace znet


#endif  // ZNET_MESSAGE_PIPELINE_H_
