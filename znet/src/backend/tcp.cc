//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/backends/tcp.h"
#include "znet/transport.h"
#include "znet/error.h"
#include "znet/peer_session.h"

namespace znet {
namespace backends {

namespace {

/** @brief Whether the last send() failed only because the buffer was full. */
inline bool WouldBlockOnSend() {
#ifdef TARGET_WIN
  // Windows reports through WSAGetLastError(); send() leaves errno untouched
  const int err = WSAGetLastError();
  return err == WSAEWOULDBLOCK || err == WSAENOBUFS;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS;
#endif
}

// how long one wait for socket room lasts before the send loop rechecks whether
// the session was closed underneath it
constexpr int kSendStallWaitMs = 50;

}  // namespace

TCPTransportLayer::TCPTransportLayer(SocketHandle socket) : socket_(socket) {
}

TCPTransportLayer::~TCPTransportLayer() {
  // where the descriptor is released; see Close() for why not there. The
  // session owning this transport is gone by now, so no worker can be inside
  // recv() on the socket.
  CloseSocket(socket_);
  socket_ = INVALID_SOCKET;
}

std::shared_ptr<Buffer> TCPTransportLayer::Receive() {
  std::shared_ptr<Buffer> new_buffer;
  if ((new_buffer = ReadBuffer())) {
    return new_buffer;
  }

  data_size_ = recv(socket_, data_ + read_offset_,
#ifdef TARGET_WIN
                    static_cast<int>(sizeof(data_) - static_cast<size_t>(read_offset_)),
#else
                    sizeof(data_) - static_cast<size_t>(read_offset_),
#endif
                    0);

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
    ZNET_METRIC(metrics_.tcp.reads++);
    ZNET_METRIC(metrics_.common.wire_bytes_received += static_cast<uint64_t>(data_size_));
    size_t full_size = static_cast<size_t>(data_size_) + static_cast<size_t>(read_offset_);
    buffer_ = std::make_shared<Buffer>(data_, full_size);
    read_offset_ = 0;
    return ReadBuffer();
  }

  if (data_size_ == -1) {
#ifdef TARGET_WIN
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
    ZNET_LOG_ERROR("Closing connection due to an error: {}", GetLastErrorInfo());
    Close();
  }
  return nullptr;
}

std::shared_ptr<Buffer> TCPTransportLayer::ReadBuffer() {
  if (!buffer_ || buffer_->readable_bytes() == 0) {
    buffer_ = nullptr;
    return nullptr;
  }
  size_t cursor = buffer_->read_cursor();
  size_t size = buffer_->ReadVarInt<size_t>();
  BufferError error = buffer_->GetAndClearLastError();
  if (error == BufferError::CorruptedFormat) {
    // the prefix itself is not a valid length, so the stream cannot be resynced.
    ZNET_LOG_ERROR("Received a corrupt frame length, closing connection!");
    Close();
    buffer_ = nullptr;
    read_offset_ = 0;
    return nullptr;
  }
  // a read can end anywhere, so either the prefix or the body may still be in
  // flight. neither is a framing error: stash the tail and wait.
  if (error == BufferError::ReadOutOfBounds || buffer_->readable_bytes() < size) {
    buffer_->set_read_cursor(cursor);
    size_t carry = buffer_->readable_bytes();
    // == is already unrecoverable: the next recv would have no room, and Send()
    // refuses frames that large anyway.
    if (carry >= sizeof(data_)) {
      ZNET_LOG_ERROR("Frame carry-over {} exceeds the buffer, closing!", carry);
      Close();
      buffer_ = nullptr;
      read_offset_ = 0;
      return nullptr;
    }
    memmove(data_, buffer_->data() + cursor, carry);
    read_offset_ = static_cast<ssize_t>(carry);
    buffer_ = nullptr;
    return nullptr;
  }
  const char* data_ptr = buffer_->read_cursor_data();
  buffer_->SkipRead(size);
  return std::make_shared<Buffer>(data_ptr, size);
}

bool TCPTransportLayer::WriteAll(Buffer& buffer) {
  // a length-prefixed stream cannot survive a dropped tail: the peer would read
  // the next frame's bytes as this one's body, so a short send is retried
  // rather than reported.
  const char* data = buffer.data();
  size_t remaining = buffer.size();
  size_t offset = 0;
  while (remaining > 0) {
    // the application closes from its own thread, so the connection can die
    // mid-frame. Send()'s check on the way in is not enough against a peer that
    // stopped reading, since this loop would spin without ever noticing.
    if (IsClosed()) {
      return false;
    }
#ifdef TARGET_WIN
    int written = send(socket_, data + offset, static_cast<int>(remaining), 0);
#else
    ssize_t written = send(socket_, data + offset, remaining, 0);
#endif
    if (written > 0) {
      offset += static_cast<size_t>(written);
      remaining -= static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && WouldBlockOnSend()) {
      if (!WaitUntilWritable(socket_, kSendStallWaitMs)) {
        ZNET_LOG_ERROR("Waiting on a stalled socket failed: {}",
                       GetLastErrorInfo());
        return false;
      }
      continue;
    }
    ZNET_LOG_ERROR("Error sending packet to the server: {}", GetLastErrorInfo());
    return false;
  }
  ZNET_METRIC(metrics_.tcp.writes++);
  ZNET_METRIC(metrics_.common.wire_bytes_sent += buffer.size());
  return true;
}

bool TCPTransportLayer::Send(std::shared_ptr<Buffer> buffer, SendOptions options) {
  (void)options;  // TCP has one stream: no channels, no ordering to choose
  if (IsClosed()) {
    ZNET_LOG_WARN("Tried to send a packet to a closed connection, dropping packet!");
    return false;
  }

  const size_t header = 48; // usually smaller than this
  const size_t limit = ZNET_MAX_BUFFER_SIZE - header;
  // the message starts at the read cursor: the send pipeline reserves headroom
  const size_t payload_size = buffer->readable_bytes();
  size_t new_size = payload_size + sizeof(size_t);
  // intentionally >= limit, not > limit
  if (new_size >= limit) {
    // ReadBuffer() reassembles within one frame buffer, so anything this large
    // could be sent but never read back
    ZNET_LOG_ERROR("Tried to send buffer size {} but the limit is {}, dropping packet!",
                   new_size, limit);
    return false;
  }

  // straight to the socket: the session already queued on the caller's behalf,
  // so there is nothing left to hand off
  Buffer framed;
  framed.ReserveExact(new_size);
  framed.WriteVarInt<size_t>(payload_size);
  framed.Write(buffer->read_cursor_data(), payload_size);
  if (!WriteAll(framed)) {
    ZNET_LOG_ERROR("TCPTransport::WriteAll failed, socket={}", socket_);
    return false;
  }
  return true;
}

// nothing to do either way: Send() writes straight to the socket, and the
// kernel owns retransmit and pacing.
void TCPTransportLayer::Flush() {}

void TCPTransportLayer::Update() {}

void TCPTransportLayer::FillMetrics(SessionMetrics& out) const {
#if ZNET_ENABLE_METRICS
  out.tcp = metrics_.tcp;
  out.common.wire_bytes_sent = metrics_.common.wire_bytes_sent;
  out.common.wire_bytes_received = metrics_.common.wire_bytes_received;
#else
  (void)out;
#endif
}

Result TCPTransportLayer::Close(CloseOptions options) {
  if (is_closed_.exchange(true, std::memory_order_acq_rel)) {
    return Result::AlreadyDisconnected;
  }
  if (options.GetOr<NoLingerKey>(false)) {
    linger l; l.l_onoff = 1; l.l_linger = 0;
    setsockopt(socket_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
  }
  // shut down but leave the descriptor open. the application closes from its
  // own thread while a worker may be inside recv() here, and close() would free
  // the number for anything in the process to reuse mid-recv. the destructor
  // releases it instead, once no worker can still be in there.
  ShutdownSocket(socket_);
  return Result::Success;
}

/*uint64_t TCPTransportLayer::GetRTT() const {
#ifndef TARGET_WIN
  struct tcp_info ti{};
  socklen_t len = sizeof(ti);
  if (getsockopt(socket_, IPPROTO_TCP, TCP_INFO, &ti, &len) == 0) {
    return ti.tcpi_rtt;
  } else {
    ZNET_LOG_ERROR("getsockopt TCP_INFO failed: {}", strerror(errno));
    return 0;
  }
#else
  return 0;
#endif
}*/

TCPClientBackend::TCPClientBackend(std::shared_ptr<InetAddress> server_address,
                                   const SessionOptions& options)
    : options_(options), server_address_(server_address) {

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
  SetTCPNoDelay(client_socket_);
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

  // the session's loop interleaves Update() (flush) with Receive(), so a
  // blocking socket would park the thread in recv() and strand everything
  // queued by Send() until the peer happened to send something back. The
  // accepted server-side sockets are already non-blocking for the same reason.
  if (!SetSocketBlocking(client_socket_, false)) {
    ZNET_LOG_ERROR("Failed to set the client socket to non-blocking: {}",
                   GetLastErrorInfo());
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
                                    std::make_unique<TCPTransportLayer>(client_socket_), ConnectionType::TCP, true,
                                    /*self_managed=*/false, options_);
  // the transport owns the descriptor now, so dropping our copy keeps
  // CleanupSocket() from closing whatever later reused that number
  client_socket_ = INVALID_SOCKET;
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
  if (client_session_) {
    client_session_->Close();
    client_session_ = nullptr;
  }
}

TCPServerBackend::TCPServerBackend(std::shared_ptr<InetAddress> bind_address,
                                   const SessionOptions& child_options)
    : child_options_(child_options), bind_address_(bind_address) {}

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
  if (!IsValidSocketHandle(server_socket_)) {
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
  if (!SetSocketBlocking(server_socket_, false)) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    CloseSocket(server_socket_);
    return Result::Failure;
  }
  SetTCPNoDelay(server_socket_);
  if (bind(server_socket_, bind_address_->handle_ptr(),
           bind_address_->addr_size()) != 0) {
    ZNET_LOG_DEBUG("Failed to bind: {}, {}", bind_address_->readable(),
                   GetLastErrorInfo());
    return Result::CannotBind;
  }
  // read the address back, so a port of 0 resolves to what was assigned
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
  SocketHandle client_socket;
  {
    // only around accept(), not the session construction below: Close() waits
    // on this, and building a PeerSession is not something to make it wait for
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_listening_) {
      return nullptr;
    }
    client_socket = accept(server_socket_, reinterpret_cast<sockaddr*>(&client_address), &addr_len);
  }
  if (!IsValidSocketHandle(client_socket)) {
    return nullptr;
  }
  if (!SetSocketBlocking(client_socket, false)) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    CloseSocket(client_socket);
    return nullptr;
  }
  SetTCPNoDelay(client_socket);
  std::shared_ptr<InetAddress> remote_address = InetAddress::from(reinterpret_cast<sockaddr*>(&client_address));
  if (remote_address == nullptr) {
    return nullptr;
  }
  return std::make_shared<PeerSession>(bind_address_, remote_address,
                                    std::make_unique<TCPTransportLayer>(client_socket), ConnectionType::TCP,
                                    /*is_initiator=*/false,
                                    /*self_managed=*/false, child_options_);
}

void TCPServerBackend::AcceptAndReject() {
  sockaddr_storage client_address{};
  socklen_t addr_len = sizeof(client_address);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_listening_) {
    return;
  }
  SocketHandle client_socket = accept(server_socket_, reinterpret_cast<sockaddr*>(&client_address), &addr_len);
  CloseSocket(client_socket);
}

bool TCPServerBackend::IsAlive() {
  return is_listening_;
}
}
}
