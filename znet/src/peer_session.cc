//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "peer_session.h"
#include "error.h"

namespace znet {

PeerSession::PeerSession(Ref<InetAddress> local_address,
                         Ref<InetAddress> remote_address,
                         SocketType socket, bool is_initiator)
    : local_address_(local_address), remote_address_(remote_address),
      socket_(socket), is_initiator_(is_initiator),
      is_alive_(true), is_ready_(false),
      transport_layer_(*this, socket),
      encryption_layer_(*this), handler_layer_(*this) {
  encryption_layer_.Initialize(is_initiator);
}

void PeerSession::Process() {
  if (!is_alive_) {
    return;
  }
  Ref<Buffer> buffer;
  if ((buffer = transport_layer_.Receive())) {
    buffer = encryption_layer_.HandleIn(buffer);
    if (buffer) {
      handler_layer_.HandleIn(buffer);
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

bool PeerSession::SendPacket(Ref<Packet> packet) {
  auto buffer = handler_layer_.HandleOut(std::move(packet));
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