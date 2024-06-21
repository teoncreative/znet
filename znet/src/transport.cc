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

  data_size_ = recv(socket_, data_ + read_offset_, sizeof(data_) - read_offset_, 0);

  if (data_size_ > ZNET_MAX_BUFFER_SIZE) {
    session_.Close();
    ZNET_LOG_ERROR(
        "Received data bigger than maximum buffer size (rx: {}, max: {}), "
        "closing connection!",
        data_size_, ZNET_MAX_BUFFER_SIZE);
    return nullptr;
  }

  if (data_size_ == 0) {
    session_.Close();
    return nullptr;
  } else if (data_size_ > 0) {
    int full_size = data_size_ + read_offset_;
    if (full_size == ZNET_MAX_BUFFER_SIZE) {
      has_more_ = true;
    } else {
      has_more_ = false;
    }
    buffer_ = CreateRef<Buffer>(data_, full_size);
    read_offset_ = 0;
    return ReadBuffer();
  } else if (data_size_ == -1) {
#ifdef WIN32
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return nullptr; // no data received
    }
    ZNET_LOG_ERROR("Closing connection due to an error: ", GetLastErrorInfo());
    session_.Close();
#else
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
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
    size_t cursor = buffer_->read_cursor();
    auto size = buffer_->ReadVarInt<size_t>();
    if (buffer_->readable_bytes() < size) {
      if (!has_more_) {
        ZNET_LOG_ERROR("Received malformed frame, closing connection!");
        session_.Close();
        return nullptr;
      }
      buffer_->set_read_cursor(cursor);
      read_offset_ = buffer_->readable_bytes();
      memcpy(data_, buffer_->data() + cursor, read_offset_);
      return nullptr;
    }
    const char* data_ptr = buffer_->data() + buffer_->read_cursor();
    buffer_->SkipRead(size);
    return CreateRef<Buffer>(data_ptr, size);
  }
  buffer_ = nullptr;
  return nullptr;
}

bool TransportLayer::Send(Ref<Buffer> buffer) {
  if (!session_.IsAlive()) {
    ZNET_LOG_WARN("Tried to send a packet to a dead connection, dropping packet!");
    return false;
  }

  const size_t header = 48; // usually smaller than this
  const size_t limit = ZNET_MAX_BUFFER_SIZE - header;
  size_t new_size = buffer->size() + sizeof(size_t);
  // This intentionally checks for >= limit, not > limit
  if (new_size >= limit) {
    // Due to the nature of how we read packets, we cannot receive packets
    // bigger than a frame (MAX_BUFFER_SIZE - HEADER_SIZE).
    ZNET_LOG_ERROR("Tried to send buffer size {} but the limit is {}, dropping packet!",
                   new_size, limit);
    return false;
  }

  auto new_buffer = CreateRef<Buffer>();
  new_buffer->ReserveExact(new_size);
  new_buffer->WriteVarInt<size_t>(buffer->size());
  new_buffer->Write(buffer->data() + buffer->read_cursor(), buffer->size());

  // todo check
  while (send(socket_, new_buffer->data(), new_buffer->size(), MSG_DONTWAIT) < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      continue;
    }
    ZNET_LOG_ERROR("Error sending packet to the server: {}", GetLastErrorInfo());
    return false;
  }
  return true;
}

} // namespace znet