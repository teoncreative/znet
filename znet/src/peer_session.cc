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

#include <utility>

namespace znet {

PeerSession::PeerSession(std::shared_ptr<InetAddress> local_address,
                         std::shared_ptr<InetAddress> remote_address,
                         std::unique_ptr<TransportLayer> transport_layer,
                         ConnectionType connection_type,
                         bool is_initiator,
                         bool self_managed)
    : local_address_(std::move(local_address)),
      remote_address_(std::move(remote_address)),
      transport_layer_(std::move(transport_layer)),
      connection_type_(connection_type),
      is_initiator_(is_initiator),
      encryption_layer_(*this),
      connect_time_(std::chrono::steady_clock::now()) {
  static SessionId sIdCount = 0;
  id_ = sIdCount++;
  encryption_layer_.Initialize(is_initiator);
  if (self_managed) {
    task_.Run([this]() {
      while (IsAlive()) {
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
  std::shared_ptr<Buffer> buffer;
  if ((buffer = transport_layer_->Receive())) {
    buffer = compr::HandleInDynamic(buffer);
    buffer = encryption_layer_.HandleIn(buffer);
    if (buffer && handler_ && codec_) {
      codec_->Deserialize(buffer, *handler_);
    }
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
#ifdef ZNET_USE_ZSTD
  SetOutCompression(CompressionType::Zstandard);
#endif
}

bool PeerSession::SendPacket(std::shared_ptr<Packet> packet, SendOptions options) {
  if (!packet || !IsAlive()) {
    return false;
  }
  auto buffer = codec_->Serialize(std::move(packet));
  if (!buffer) {
    return false;
  }
  buffer = encryption_layer_.HandleOut(std::move(buffer));
  if (!buffer) {
    return false;
  }
  buffer = compr::HandleOutWithType(out_compression_type_, std::move(buffer));
  if (!buffer) {
    return false;
  }
  return transport_layer_->Send(buffer, options);
}

bool PeerSession::SendRaw(std::shared_ptr<Buffer> buffer, SendOptions options) {
  if (!buffer || !IsAlive()) {
    return false;
  }
  return transport_layer_->Send(buffer, options);
}

}