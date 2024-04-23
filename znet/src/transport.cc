//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "transport.h"
#include "error.h"
#include "peer_session.h"

namespace znet {

TransportLayer::TransportLayer(znet::PeerSession& session, znet::SocketType socket) :
  session_(session), socket_(socket) {

}

TransportLayer::~TransportLayer() {

}

Ref<Buffer> TransportLayer::Receive() {
  Ref<Buffer> new_buffer;
  if ((new_buffer = ReadBuffer())) {
    return new_buffer;
  }

  // Handle the client connection
  // since recv is in blocking mode here, locking the mutex would cause
  // it to be locked until the data is received. which means if
  // client is the one initiating the connection, it will be stuck
  //mutex_.lock();
  data_size_ = recv(socket_, data_, sizeof(data_), 0);
  //mutex_.unlock();

  if (data_size_ > MAX_BUFFER_SIZE) {
    session_.Close();
    ZNET_LOG_ERROR(
        "Received data bigger than maximum buffer size (rx: {}, max: {}), "
        "closing connection!",
        data_size_, MAX_BUFFER_SIZE);
    return nullptr;
  }

  if (data_size_ == 0) {
    session_.Close();
    return nullptr;
  } else if (data_size_ > 0) {
    buffer_ = CreateRef<Buffer>(data_, data_size_);
    return ReadBuffer();
  } else if (data_size_ == -1) {
#ifdef WIN32
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return; // no data received
    }
    ZNET_LOG_ERROR("Closing connection due to an error: ", GetLastErrorInfo());
    Close();
#else
    if (errno == EWOULDBLOCK) {
      return nullptr; // no data received
    }
    ZNET_LOG_ERROR("Closing connection due to an error: ", GetLastErrorInfo());
    session_.Close();
#endif
  }
  return nullptr;
}

Ref<Buffer> TransportLayer::ReadBuffer() {
  if (buffer_ && buffer_->readable_bytes() > 0) {
    size_t size = buffer_->ReadInt<size_t>();
    if (buffer_->readable_bytes() < size) {
      ZNET_LOG_ERROR("Received malformed frame, closing connection!");
      session_.Close();
      return nullptr;
    }
    char* data_ptr = const_cast<char*>(buffer_->data()) + buffer_->read_cursor();
    buffer_->SkipRead(size);
    return CreateRef<Buffer>(data_ptr, size);
  }
  buffer_ = nullptr;
  return nullptr;
}

void TransportLayer::Send(Ref<Buffer> buffer) {
  if (!session_.IsAlive()) {
    ZNET_LOG_WARN("Tried to send a packet to a dead connection, buffer will be dropped!");
    return;
  }

  auto new_buffer = CreateRef<Buffer>();
  new_buffer->ReserveExact(buffer->size() + sizeof(size_t));
  new_buffer->WriteInt<size_t>(buffer->size());
  new_buffer->Write(buffer->data() + buffer->read_cursor(), buffer->size());

  if (send(socket_, new_buffer->data(), new_buffer->size(), 0) < 0) {
    ZNET_LOG_ERROR("Error sending data to the server: {}", GetLastErrorInfo());
    return;
  }
}

} // namespace znet