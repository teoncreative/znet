//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <utility>

#include "znet/peer_session.h"
#include "znet/server_events.h"
#include "znet/error.h"

namespace znet {

PeerSession::PeerSession(std::shared_ptr<InetAddress> local_address,
                         std::shared_ptr<InetAddress> remote_address,
                         SocketHandle socket, bool is_initiator)
    : local_address_(std::move(local_address)),
      remote_address_(std::move(remote_address)),
      socket_(socket), is_initiator_(is_initiator),
      transport_layer_(*this, socket),
      encryption_layer_(*this),
      connect_time_(std::chrono::steady_clock::now()) {
  static SessionId sIdCount = 0;
  id_ = sIdCount++;
  encryption_layer_.Initialize(is_initiator);
}

void PeerSession::Process() {
  if (!is_alive_) {
    return;
  }
  if (IsExpired()) {
    ZNET_LOG_INFO("Session {} was expired!", id_);
    Close();
    return;
  }
  std::shared_ptr<Buffer> buffer;
  if ((buffer = transport_layer_.Receive())) {
    buffer = compr::HandleInDynamic(buffer);
    buffer = encryption_layer_.HandleIn(buffer);
    if (buffer && handler_ && codec_) {
      codec_->Deserialize(buffer, *handler_);
    }
  }
}

Result PeerSession::Close() {
  if (!is_alive_) {
    return Result::AlreadyDisconnected;
  }
  // Close the socket
  is_alive_ = false;
#ifdef TARGET_WIN
  closesocket(socket_);
#else
  close(socket_);
#endif
  return Result::Success;
}

void PeerSession::Ready() {
  is_ready_ = true;
  connect_time_ = std::chrono::steady_clock::now();
  SetOutCompression(CompressionType::Zstandard);
#ifdef ZNET_USE_ZSTD
#endif
}
bool PeerSession::SendPacket(std::shared_ptr<Packet> packet) {
  auto buffer = codec_->Serialize(std::move(packet));
  if (!buffer) {
    return false;
  }
  buffer = encryption_layer_.HandleOut(std::move(buffer));
  buffer = compr::HandleOutWithType(out_compression_type_, std::move(buffer));
  if (!buffer) {
    return false;
  }
  return transport_layer_.Send(buffer);
}

}