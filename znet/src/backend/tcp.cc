//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 06/08/2025.
//

#include "znet/backends/tcp.h"
#include "znet/transport.h"
#include "znet/error.h"
#include "znet/peer_session.h"

namespace znet {
namespace backends {


TCPTransportLayer::TCPTransportLayer(SocketHandle socket) : socket_(socket) {

}

TCPTransportLayer::~TCPTransportLayer() {

}

std::shared_ptr<Buffer> TCPTransportLayer::Receive() {
  std::shared_ptr<Buffer> new_buffer;
  if ((new_buffer = ReadBuffer())) {
    return new_buffer;
  }

  data_size_ = recv(socket_, data_ + read_offset_, sizeof(data_) - read_offset_, 0);

  if (data_size_ > ZNET_MAX_BUFFER_SIZE) {
    Close();
    ZNET_LOG_ERROR(
        "Received data bigger than maximum buffer size (rx: {}, max: {}), "
        "closing connection!",
        data_size_, ZNET_MAX_BUFFER_SIZE);
    return nullptr;
  }

  if (data_size_ == 0) {
    Close();
    return nullptr;
  }

  if (data_size_ > 0) {
    int full_size = data_size_ + read_offset_;
    if (full_size == ZNET_MAX_BUFFER_SIZE) {
      has_more_ = true;
    } else {
      has_more_ = false;
    }
    buffer_ = std::make_shared<Buffer>(data_, full_size);
    read_offset_ = 0;
    return ReadBuffer();
  }

  if (data_size_ == -1) {
#ifdef WIN32
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return nullptr; // no data received
    }
    if (err == WSAECONNRESET) {
      ZNET_LOG_ERROR("Connection lost because peer has closed the connection.");
      Close();
      return nullptr;
    }
#else
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return nullptr; // no data received
    }
    if (errno == ECONNRESET) {
      ZNET_LOG_ERROR("Closing connection because peer closed the connection.");
      Close();
      return nullptr;
    }
#endif
    ZNET_LOG_ERROR("Closing connection due to an error: ", GetLastErrorInfo());
    Close();
  }
  return nullptr;
}

std::shared_ptr<Buffer> TCPTransportLayer::ReadBuffer() {
  if (buffer_ && buffer_->readable_bytes() > 0) {
    size_t cursor = buffer_->read_cursor();
    auto size = buffer_->ReadVarInt<size_t>();
    if (buffer_->readable_bytes() < size) {
      if (!has_more_) {
        ZNET_LOG_ERROR("Received malformed frame, closing connection!");
        Close();
        return nullptr;
      }
      buffer_->set_read_cursor(cursor);
      read_offset_ = buffer_->readable_bytes();
      memcpy(data_, buffer_->data() + cursor, read_offset_);
      return nullptr;
    }
    const char* data_ptr = buffer_->read_cursor_data();
    buffer_->SkipRead(size);
    return std::make_shared<Buffer>(data_ptr, size);
  }
  buffer_ = nullptr;
  return nullptr;
}

bool TCPTransportLayer::Send(std::shared_ptr<Buffer> buffer, SendOptions options) {
  if (IsClosed()) {
    ZNET_LOG_WARN("Tried to send a packet to a closed connection, dropping packet!");
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

  auto new_buffer = std::make_shared<Buffer>();
  new_buffer->ReserveExact(new_size);
  new_buffer->WriteVarInt<size_t>(buffer->size());
  new_buffer->Write(buffer->data() + buffer->read_cursor(), buffer->size());

  // todo check
  while (send(socket_, new_buffer->data(), new_buffer->size(), 0) < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      continue;
    }
    ZNET_LOG_ERROR("Error sending packet to the server: {}", GetLastErrorInfo());
    return false;
  }
  return true;
}

Result TCPTransportLayer::Close() {
  if (is_closed_) {
    return Result::AlreadyDisconnected;
  }
  // Close the socket
  is_closed_ = true;
  CloseSocket(socket_);
  socket_ = INVALID_SOCKET;
  return Result::Success;
}

TCPClientBackend::TCPClientBackend(std::shared_ptr<InetAddress> server_address)
    : server_address_(server_address) {

}

TCPClientBackend::~TCPClientBackend() {
  ZNET_LOG_DEBUG("Destructor of the TCP client backend is called.");
  Close();
}

Result TCPClientBackend::Bind() {
  client_socket_ = socket(GetDomainByInetProtocolVersion(server_address_->ipv()), SOCK_STREAM, 0);
  if (!IsValidSocketHandle(client_socket_)) {
    ZNET_LOG_ERROR("Error binding socket.");
    return Result::CannotBind;
  }
  const char option = 1;
#ifdef TARGET_WIN
  setsockopt(client_socket_, SOL_SOCKET, SO_BROADCAST, &option,
             sizeof(option));
  setsockopt(client_socket_, SOL_SOCKET, SO_BROADCAST, &option,
             sizeof(option));
#else
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
  setsockopt(client_socket_, SOL_SOCKET, SO_BROADCAST, &option, sizeof(option));
#endif
  is_bind_ = true;
  return Result::Success;
}

Result TCPClientBackend::Bind(const std::string& ip, PortNumber port) {
  Result result = Bind();
  if (result != Result::Success) {
    return result;
  }
  local_address_ = InetAddress::from(ip, port);
  if (bind(client_socket_, local_address_->handle_ptr(), local_address_->addr_size()) != 0) {
    ZNET_LOG_DEBUG("Failed to bind: {}, {}", local_address_->readable(),
                   GetLastErrorInfo());
    CleanupSocket();
    return Result::CannotBind;
  }
  return Result::Success;
}

Result TCPClientBackend::Connect() {
  if (client_session_ && client_session_->IsAlive()) {
    return Result::AlreadyConnected;
  }
  if (!server_address_ || !server_address_->is_valid()) {
    return Result::InvalidRemoteAddress;
  }
  if (!is_bind_) {
    ZNET_LOG_ERROR("Cannot connect because the client is not bound, make sure to call Bind() first.");
    return Result::CannotBind;
  }
  if (connect(client_socket_, server_address_->handle_ptr(),
              server_address_->addr_size()) < 0) {
    ZNET_LOG_ERROR("Error connecting to server: {}", GetLastErrorInfo());
    CleanupSocket();
    return Result::Failure;
  }

  sockaddr_storage local_ss{};
  socklen_t local_len = sizeof(local_ss);
  if (getsockname(client_socket_, reinterpret_cast<sockaddr*>(&local_ss), &local_len) == 0) {
    local_address_ = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
  } else {
    ZNET_LOG_ERROR("getsockname failed, local address will be nullptr: {}", GetLastErrorInfo());
  }

  client_session_ =
      std::make_shared<PeerSession>(local_address_, server_address_,
                                    std::make_unique<TCPTransportLayer>(client_socket_), true);
  return Result::Success;
}

Result TCPClientBackend::Close() {
  if (!client_session_) {
    return Result::AlreadyClosed;
  }
  return client_session_->Close();
}

void TCPClientBackend::Update() {

}

bool TCPClientBackend::IsAlive() {
  return client_session_ && client_session_->IsAlive();
}

void TCPClientBackend::CleanupSocket() {
  CloseSocket(client_socket_);
  client_socket_ = INVALID_SOCKET;
  is_bind_ = false;
  client_session_->Close();
  client_session_ = nullptr;
}

TCPServerBackend::TCPServerBackend(std::shared_ptr<InetAddress> bind_address)
    : bind_address_(bind_address) {
}

TCPServerBackend::~TCPServerBackend() {
  ZNET_LOG_DEBUG("Destructor of the TCP server backend is called.");
  Close();
}

Result TCPServerBackend::Bind() {
  if (is_bind_) {
    return Result::AlreadyBound;
  }
  if (!bind_address_ || !bind_address_->is_valid()) {
    return Result::InvalidAddress;
  }

  const char option = 1;
  int domain = GetDomainByInetProtocolVersion(bind_address_->ipv());
  server_socket_ = socket(
      domain, SOCK_STREAM,
      0);  // SOCK_STREAM for TCP, SOCK_DGRAM for UDP, there is also SOCK_RAW,
           // but we don't care about that.
  if (server_socket_ == -1) {
    ZNET_LOG_ERROR("Error creating socket. {}", GetLastErrorInfo());
    return Result::CannotCreateSocket;
  }
#ifdef TARGET_WIN
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, &option,
             sizeof(option));
#else
  setsockopt(server_socket_, SOL_SOCKET,
             SO_REUSEADDR | SO_REUSEPORT | SO_BROADCAST, &option,
             sizeof(option));
#endif
  // Enable non-blockig socket
  if (!SetSocketBlocking(server_socket_, false)) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    CloseSocket(server_socket_);
    return Result::Failure;
  }

  if (bind(server_socket_, bind_address_->handle_ptr(),
           bind_address_->addr_size()) != 0) {
    ZNET_LOG_DEBUG("Failed to bind: {}, {}", bind_address_->readable(),
                   GetLastErrorInfo());
    return Result::CannotBind;
  }
  // Get the bind address back so we know the actual port.
  sockaddr_storage local_ss{};
  socklen_t local_len = sizeof(local_ss);
  if (getsockname(server_socket_, reinterpret_cast<sockaddr*>(&local_ss), &local_len) == 0) {
    bind_address_ = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
  } else {
    ZNET_LOG_ERROR("getsockname failed: {}", GetLastErrorInfo());
  }
  is_bind_ = true;
  ZNET_LOG_DEBUG("Bind to: {}", bind_address_->readable());
  return Result::Success;
}

Result TCPServerBackend::Listen() {
  if (is_listening_) {
    return Result::AlreadyListening;
  }
  if (!is_bind_) {
    ZNET_LOG_ERROR("Cannot listen because the server is not bound, make sure to call Bind() first.");
    return Result::NotBound;
  }
  if (listen(server_socket_, SOMAXCONN) != 0) {
    ZNET_LOG_DEBUG("Failed to listen connections from: {}, {}",
                   bind_address_->readable(), GetLastErrorInfo());
    return Result::CannotListen;
  }
  is_listening_ = true;
  return Result::Success;
}

Result TCPServerBackend::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_listening_) {
    return Result::AlreadyStopped;
  }
  // Close the server
  if (!CloseSocket(server_socket_)) {
    ZNET_LOG_DEBUG("Failed to close socket: {}, {}",
                   bind_address_->readable(), GetLastErrorInfo());
  }
  is_listening_ = false;
  is_bind_ = false;
  return Result::Success;
}

void TCPServerBackend::Update() {

}

std::shared_ptr<PeerSession> TCPServerBackend::Accept() {
  sockaddr_storage client_address{};
  socklen_t addr_len = sizeof(client_address);
  SocketHandle client_socket = accept(server_socket_, reinterpret_cast<sockaddr*>(&client_address), &addr_len);
  if (!IsValidSocketHandle(client_socket)) {
    return nullptr;
  }
#ifdef TARGET_WIN
  u_long mode = 1;  // 1 to enable non-blocking socket
  ioctlsocket(server_socket_, FIONBIO, &mode);
#else
  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags < 0) {
    ZNET_LOG_ERROR("Error getting socket flags: {}", GetLastErrorInfo());
    CloseSocket(client_socket);
    return nullptr;
  }
  if (fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    CloseSocket(client_socket);
    return nullptr;
  }
#endif
  std::shared_ptr<InetAddress> remote_address = InetAddress::from(reinterpret_cast<sockaddr*>(&client_address));
  if (remote_address == nullptr) {
    return nullptr;
  }
  return std::make_shared<PeerSession>(bind_address_, remote_address,
                                    std::make_unique<TCPTransportLayer>(client_socket));
}

void TCPServerBackend::AcceptAndReject() {
  sockaddr_storage client_address{};
  socklen_t addr_len = sizeof(client_address);
  SocketHandle client_socket = accept(server_socket_, reinterpret_cast<sockaddr*>(&client_address), &addr_len);
  CloseSocket(client_socket);
}

bool TCPServerBackend::IsAlive() {
  return is_listening_;
}

std::unique_ptr<ClientBackend> CreateClientFromType(ConnectionType type,
                                                    std::shared_ptr<InetAddress> server_address) {
  if (type == ConnectionType::TCP) {
    return std::make_unique<TCPClientBackend>(server_address);
  }
  return nullptr;
}

std::unique_ptr<ServerBackend> CreateServerFromType(ConnectionType type,
                                                    std::shared_ptr<InetAddress> bind_address) {
  if (type == ConnectionType::TCP) {
    return std::make_unique<TCPServerBackend>(bind_address);
  }
  return nullptr;
}

}
}
