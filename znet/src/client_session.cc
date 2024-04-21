//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "client_session.h"
#include "error.h"
#include "logger.h"

namespace znet {

ClientSession::ClientSession(Ref<InetAddress> local_address,
                             Ref<InetAddress> remote_address, SocketType socket)
    : ConnectionSession(local_address, remote_address), socket_(socket) {
  is_alive_ = true;
  memset(&buffer_, 0, MAX_BUFFER_SIZE);
  encryption_layer_.Initialize(true);
}

void ClientSession::Process() {
  if (!is_alive_) {
    return;
  }
  // Handle the client connection
  mutex_.lock();
  data_size_ = recv(socket_, buffer_, sizeof(buffer_), 0);
  mutex_.unlock();

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
  } else if (data_size_ > 0) {
    auto buffer = CreateRef<Buffer>(buffer_, data_size_);
    buffer = encryption_layer_.HandleIn(buffer);
    if (buffer) {
      handler_layer_.HandleIn(buffer);
    }
  }

#ifdef WIN32
  else if (data_size_ == -1) {
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return; // no data received
    }
    ZNET_LOG_ERROR("Closing connection due to an error. (error code: {})", err);
    Close();
  }
#endif
}

Result ClientSession::Close() {
  std::scoped_lock lock(mutex_);
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

void ClientSession::SendPacket(Ref<Packet> packet) {
  auto buffer = handler_layer_.HandleOut(packet);
  if (buffer) {
    SendRaw(buffer);
  }
}

void ClientSession::SendRaw(Ref<Buffer> buffer) {
  buffer = encryption_layer_.HandleOut(buffer);
  if (!buffer) {
    return;
  }
  std::scoped_lock lock(mutex_);
  if (!is_alive_) {
    return;
  }
  if (send(socket_, buffer->data(), buffer->size(), 0) < 0) {
    ZNET_LOG_ERROR("Error sending data to the server: {}", GetLastErrorInfo());
    return;
  }
}


}  // namespace znet