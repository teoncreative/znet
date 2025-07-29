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
#include "znet/server_events.h"
#include "znet/error.h"

namespace znet {

PeerSession::PeerSession(std::shared_ptr<InetAddress> local_address,
                         std::shared_ptr<InetAddress> remote_address,
                         SocketHandle socket, bool is_initiator)
    : local_address_(local_address), remote_address_(remote_address),
      socket_(socket), is_initiator_(is_initiator),
      is_alive_(true), is_ready_(false),
      transport_layer_(*this, socket),
      encryption_layer_(*this) {
  static SessionId sIdCount = 0;
  session_id_ = sIdCount++;
  encryption_layer_.Initialize(is_initiator);
}

void PeerSession::Process() {
  if (!is_alive_) {
    return;
  }
  std::shared_ptr<Buffer> buffer;
  if ((buffer = transport_layer_.Receive())) {
    buffer = encryption_layer_.HandleIn(buffer);
    if (buffer && handler_) {
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

bool PeerSession::SendPacket(std::shared_ptr<Packet> packet) {
  auto buffer = codec_->Serialize(std::move(packet));
  if (!buffer) {
    return false;
  }
  buffer = encryption_layer_.HandleOut(std::move(buffer));
  if (!buffer) {
    return false;
  }
  return transport_layer_.Send(buffer);
}

}