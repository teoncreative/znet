//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/base/server_session.h"
#include "znet/logger.h"

namespace znet {
ServerSession::ServerSession(Ref<InetAddress> local_address,
                             Ref<InetAddress> remote_address, SocketType socket)
    : ConnectionSession(local_address, remote_address), socket_(socket) {
  is_alive_ = true;
  memset(&buffer_, 0, MAX_BUFFER_SIZE);
}

void ServerSession::Process() {
  if (!is_alive_) {
    return;
  }

  // Handle the client connection
  data_size_ = recv(socket_, buffer_, sizeof(buffer_), 0);
  if (data_size_ > MAX_BUFFER_SIZE) {
    Close();
    ZNET_LOG_ERROR(
        "Received data bigger than maximum buffer size (rx: {}, max: {}), "
        "closing connection!",
        data_size_, MAX_BUFFER_SIZE);
    return;
  }
  if (data_size_ == 0) {
    Close();
    return;
  } else if (data_size_ > 0) {
    auto buffer = CreateRef<Buffer>(buffer_, data_size_);
    handler_layer_.Handle(*this, buffer);
  }
}

void ServerSession::Close() {
  // Close the client socket
#ifdef TARGET_WIN
  closesocket(socket_);
#else
  close(socket_);
#endif
  is_alive_ = false;
}

bool ServerSession::IsAlive() {
  return is_alive_;
}

void ServerSession::SendPacket(Ref<Packet> packet) {
  auto buffer = handler_layer_.Serialize(*this, packet);
  if (buffer) {
    SendRaw(buffer);
  }
}

void ServerSession::SendRaw(Ref<Buffer> buffer) {
  if (send(socket_, buffer->data(), buffer->Size(), 0) < 0) {
    ZNET_LOG_ERROR("Error sending data to the server.");
    return;
  }
}
}  // namespace znet