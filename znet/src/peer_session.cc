//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/peer_session.h"
#include "znet/scheduler.h"
#include "znet/error.h"

#include <atomic>
#include <utility>

namespace znet {

PeerSession::PeerSession(std::shared_ptr<InetAddress> local_address,
                         std::shared_ptr<InetAddress> remote_address,
                         std::unique_ptr<TransportLayer> transport_layer,
                         ConnectionType connection_type,
                         bool is_initiator,
                         bool self_managed,
                         const SessionOptions& options)
    : local_address_(std::move(local_address)),
      remote_address_(std::move(remote_address)),
      connection_type_(connection_type),
      transport_layer_(std::move(transport_layer)),
      encryption_layer_(*this),
      options_(options),
      is_initiator_(is_initiator),
      connect_time_(std::chrono::steady_clock::now()) {
  // sessions are minted on whichever thread accepted or dialed them, so this
  // counter is shared across threads.
  static std::atomic<SessionId> sIdCount{1};
  id_ = sIdCount.fetch_add(1, std::memory_order_relaxed);
  // only the accepting side's options count; an initiator adopts whatever the
  // server announces at handshake
  negotiated_compression_ =
      is_initiator ? CompressionType::None
                   : ResolveCompressionType(options_.common.compression);
  encryption_layer_.Initialize(is_initiator, options_.common.encryption);
  if (self_managed) {
    task_.Run([this]() {
      while (IsAlive() && !task_.IsStopRequested()) {
        Process();
      }
    });
  }
}

PeerSession::~PeerSession() {
  Close();
}

void PeerSession::Process() {
  if (!IsAlive()) {
    return;
  }
  if (IsExpired()) {
    ZNET_LOG_INFO("Session {} was expired!", id_);
    Close();
    return;
  }
  transport_layer_->Update();
  // drain what is already buffered rather than one message per tick, otherwise
  // throughput is capped at the caller's tick rate. The bound keeps one busy
  // session from starving the others sharing this worker.
  std::shared_ptr<Buffer> buffer;
  for (uint32_t i = 0; i < kMaxReceivesPerProcess; i++) {
    buffer = transport_layer_->Receive();
    if (!buffer) {
      break;
    }
    // mirror of the send path: decrypt, then decompress
    buffer = encryption_layer_.HandleIn(buffer);
    if (!buffer) {
      ZNET_LOG_ERROR("Session {} decryption returned null!", id_);
      continue;
    }
    buffer = compr::HandleInDynamic(buffer);
    if (!buffer) {
      ZNET_LOG_ERROR("Session {} decompression returned null!", id_);
      continue;
    }
    if (handler_ && codec_) {
      ZNET_METRIC(metrics_.common.messages_received++);
      ZNET_METRIC(metrics_.common.message_bytes_received += buffer->size());
      codec_->Deserialize(buffer, *handler_);
    }
  }
  // handlers above almost always answer, and Update() already ran, so without
  // this their replies would sit in the queue until the next tick and every
  // round trip would cost two.
  if (IsAlive()) {
    transport_layer_->Flush();
  }
}

Result PeerSession::Close(CloseOptions options) {
  if (!transport_layer_) {
    return Result::InvalidTransport;
  }
  return transport_layer_->Close(options);
}

bool PeerSession::IsAlive() {
  return transport_layer_ && !transport_layer_->IsClosed();
}

void PeerSession::Ready() {
  if (!IsAlive()) {
    return;
  }
  is_ready_ = true;
  connect_time_ = std::chrono::steady_clock::now();
  if (negotiated_compression_ != CompressionType::None) {
    SetOutCompression(negotiated_compression_);
  }
}

bool PeerSession::SendPacket(std::shared_ptr<Packet> packet, SendOptions options) {
  if (!packet || !IsAlive()) {
    return false;
  }
  auto buffer = codec_->Serialize(std::move(packet));
  if (!buffer) {
    return false;
  }
  // compress before encrypting; ciphertext is incompressible, so the other
  // order costs a full pass and saves nothing. Small messages skip it: the
  // coder tables cost more than they can ever save back.
  CompressionType compression = out_compression_type_;
  if (buffer->size() < options_.common.compression_threshold) {
    compression = CompressionType::None;
  }
  buffer = compr::HandleOutWithType(compression, std::move(buffer));
  if (!buffer) {
    return false;
  }
  buffer = encryption_layer_.HandleOut(std::move(buffer));
  if (!buffer) {
    return false;
  }
  ZNET_METRIC(metrics_.common.message_bytes_sent += buffer->size());
  if (!transport_layer_->Send(buffer, options)) {
    ZNET_METRIC(metrics_.common.send_failures++);
    return false;
  }
  ZNET_METRIC(metrics_.common.messages_sent++);
  return true;
}

bool PeerSession::SendRaw(std::shared_ptr<Buffer> buffer, SendOptions options) {
  if (!buffer || !IsAlive()) {
    return false;
  }
  ZNET_METRIC(metrics_.common.message_bytes_sent += buffer->size());
  if (!transport_layer_->Send(buffer, options)) {
    ZNET_METRIC(metrics_.common.send_failures++);
    return false;
  }
  ZNET_METRIC(metrics_.common.messages_sent++);
  return true;
}

}