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

#ifndef ZNET_TARGET_WIN
#include <poll.h>
#endif

#include <algorithm>
#include <thread>
#include "znet/transport.h"
#include "znet/error.h"
#include "znet/peer_session.h"
#include "znet/util.h"

namespace znet {
namespace backends {

namespace {

/** @brief Whether the last send() failed only because the buffer was full. */
inline bool WouldBlockOnSend() {
#ifdef ZNET_TARGET_WIN
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

TCPTransportLayer::TCPTransportLayer(SocketHandle socket, CommonOptions common)
    : socket_(socket),
      keepalive_interval_(common.keepalive_interval),
      idle_timeout_(common.idle_timeout),
      last_recv_(std::chrono::steady_clock::now()),
      last_send_(std::chrono::steady_clock::now()) {
  // one reservation for the connection's lifetime; recv() is bounded by the
  // space left in it, so it never grows
  recv_buffer_.ReserveExact(ZNET_MAX_BUFFER_SIZE);
#if defined(ZNET_TARGET_APPLE)
  // no MSG_NOSIGNAL on Apple; this is the per-socket equivalent, so a send
  // into a reset connection reports EPIPE instead of raising SIGPIPE
  int no_sigpipe = 1;
  setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE,
             reinterpret_cast<const char*>(&no_sigpipe), sizeof(no_sigpipe));
#endif
}

TCPTransportLayer::~TCPTransportLayer() {
  // where the descriptor is released; see Close() for why not there. The
  // session owning this transport is gone by now, so no worker can be inside
  // recv() on the socket.
  CloseSocket(socket_);
  socket_ = kSocketInvalid;
}

std::shared_ptr<Buffer> TCPTransportLayer::Receive() {
  std::shared_ptr<Buffer> new_buffer;
  if ((new_buffer = ReadBuffer())) {
    return new_buffer;
  }

  // ReadBuffer() compacted, so everything past the write cursor is free to
  // append into. A partial frame can never fill the reservation (see the
  // oversize check), so there is always room to make progress.
  ssize_t received = SocketRecv(socket_, recv_buffer_.write_cursor_data(),
                                recv_buffer_.writable_bytes());

  if (received == 0) {
    Close();
    return nullptr;
  }

  if (received > 0) {
    last_recv_ = std::chrono::steady_clock::now();
    ZNET_METRIC(metrics_.tcp.reads++);
    ZNET_METRIC(metrics_.common.wire_bytes_received += static_cast<uint64_t>(received));
    recv_buffer_.CommitWrite(static_cast<size_t>(received));
    return ReadBuffer();
  }

  if (received == -1) {
#ifdef ZNET_TARGET_WIN
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
  // control frames are consumed in place, so this loops until it has a data
  // frame to hand up or runs out of complete frames. under two readable bytes
  // the length prefix itself is still in flight.
  while (recv_buffer_.readable_bytes() >= 2) {
    const size_t frame_start = recv_buffer_.read_cursor();
    // a fixed big-endian uint16, which is the buffer's endianness: cheap to
    // parse, cheap to prepend, and a frame is bounded far below what it can
    // express
    const size_t size = recv_buffer_.ReadInt<uint16_t>();
    if (size + 2 > recv_buffer_.capacity()) {
      // could never be completed, let alone have been sent by Send(). this is
      // also what keeps a partial frame from deadlocking a full buffer: any
      // frame that passes always fits alongside its prefix, so recv() always
      // has room to complete it.
      ZNET_LOG_ERROR("Received an oversized frame length {}, closing!", size);
      Close();
      recv_buffer_.Reset();
      return nullptr;
    }
    // a zero length is a control frame; its body is the one byte that follows
    const size_t need = size == 0 ? 1 : size;
    // a read can end anywhere, so the body may still be in flight. not a
    // framing error: rewind the prefix and let the next recv() complete it.
    if (recv_buffer_.readable_bytes() < need) {
      recv_buffer_.set_read_cursor(frame_start);
      break;
    }
    if (size == 0) {
      HandleControl(recv_buffer_.ReadInt<uint8_t>());
      continue;
    }
    auto frame =
        std::make_shared<Buffer>(recv_buffer_.read_cursor_data(), size);
    recv_buffer_.SkipRead(size);
    return frame;
  }
  // reclaim the consumed front so the next recv() appends after the tail
  recv_buffer_.Compact();
  return nullptr;
}

void TCPTransportLayer::HandleControl(uint8_t type) {
  if (type == kControlPing) {
    SendControl(kControlPong);
  }
}

void TCPTransportLayer::SendControl(uint8_t type) {
  if (IsClosed()) {
    return;
  }
  Buffer frame;
  frame.ReserveExact(3);
  frame.WriteInt<uint8_t>(0);  // a zero uint16 length marks a control frame
  frame.WriteInt<uint8_t>(0);
  frame.WriteInt<uint8_t>(type);
  WriteAll(frame);
}

bool TCPTransportLayer::WriteAll(Buffer& buffer) {
  // one frame reaches the socket at a time, whoever produced it; see
  // write_mutex_
  std::lock_guard<std::mutex> lock(write_mutex_);
  // a length-prefixed stream cannot survive a dropped tail: the peer would read
  // the next frame's bytes as this one's body, so a short send is retried
  // rather than reported. The frame is the readable region: a buffer framed
  // in place still carries unspent headroom in front of it.
  const char* data = buffer.read_cursor_data();
  size_t remaining = buffer.readable_bytes();
  size_t offset = 0;
  while (remaining > 0) {
    // the application closes from its own thread, so the connection can die
    // mid-frame. Send()'s check on the way in is not enough against a peer that
    // stopped reading, since this loop would spin without ever noticing.
    if (IsClosed()) {
      return false;
    }
    ssize_t written = SocketSend(socket_, data + offset, remaining);
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
  last_send_ = std::chrono::steady_clock::now();
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
  if (payload_size == 0) {
    // a zero length prefix is a control frame; an empty message would read
    // back as one
    ZNET_LOG_WARN("Tried to send an empty buffer, dropping packet!");
    return false;
  }
  size_t new_size = payload_size + 2;
  // intentionally >= limit, not > limit
  if (new_size >= limit || payload_size > 0xFFFF) {
    // ReadBuffer() reassembles within one frame buffer, so anything this large
    // could be sent but never read back
    ZNET_LOG_ERROR("Tried to send buffer size {} but the limit is {}, dropping packet!",
                   new_size, limit);
    return false;
  }

  // straight to the socket: the session already queued on the caller's behalf,
  // so there is nothing left to hand off. The frame goes in place when the
  // pipeline left headroom; only a foreign buffer costs a copy.
  const uint8_t high = static_cast<uint8_t>(payload_size >> 8);
  const uint8_t low = static_cast<uint8_t>(payload_size & 0xFF);
  if (buffer->read_cursor() >= 2) {
    buffer->PrependInt8(low);
    buffer->PrependInt8(high);
    if (!WriteAll(*buffer)) {
      ZNET_LOG_ERROR("TCPTransport::WriteAll failed, socket={}", socket_);
      return false;
    }
    return true;
  }
  Buffer framed;
  framed.ReserveExact(new_size);
  framed.WriteInt<uint8_t>(high);
  framed.WriteInt<uint8_t>(low);
  framed.Write(buffer->read_cursor_data(), payload_size);
  if (!WriteAll(framed)) {
    ZNET_LOG_ERROR("TCPTransport::WriteAll failed, socket={}", socket_);
    return false;
  }
  return true;
}

// nothing to do: Send() writes straight to the socket, and the kernel owns
// retransmit and pacing.
void TCPTransportLayer::Flush() {}

void TCPTransportLayer::Update() {
  if (IsClosed()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (idle_timeout_.count() > 0 && now - last_recv_ > idle_timeout_) {
    ZNET_LOG_WARN("TCP: closing socket {} due to idle timeout", socket_);
    Close();
    return;
  }
  if (keepalive_interval_.count() > 0) {
    std::chrono::steady_clock::time_point last_send;
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      last_send = last_send_;
    }
    if (now - last_send > keepalive_interval_) {
      SendControl(kControlPing);
    }
  }
}

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
#ifndef ZNET_TARGET_WIN
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
  // none of these options mean anything on a Unix socket
  if (server_address_->ipv() != InetProtocolVersion::Unix) {
    SetTCPNoDelay(client_socket_);
    const char option = 1;
#ifdef ZNET_TARGET_WIN
    setsockopt(client_socket_, SOL_SOCKET, SO_BROADCAST, &option,
               sizeof(option));
    setsockopt(client_socket_, SOL_SOCKET, SO_BROADCAST, &option,
               sizeof(option));
#else
    setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    setsockopt(client_socket_, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
    setsockopt(client_socket_, SOL_SOCKET, SO_BROADCAST, &option, sizeof(option));
#endif
  }
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

  wait_socket_ = client_socket_;
  client_session_ =
      std::make_shared<PeerSession>(local_address_, server_address_,
                                    std::make_unique<TCPTransportLayer>(client_socket_, options_.common), ConnectionType::TCP, true,
                                    /*self_managed=*/false, options_);
  // the transport owns the descriptor now, so dropping our copy keeps
  // CleanupSocket() from closing whatever later reused that number
  client_socket_ = kSocketInvalid;
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

void TCPClientBackend::WaitReadable(std::chrono::milliseconds timeout) {
  if (!IsValidSocketHandle(wait_socket_)) {
    std::this_thread::sleep_for(timeout);
    return;
  }
  pollfd entry{};
  entry.fd = wait_socket_;
  entry.events = POLLIN;
#ifdef ZNET_TARGET_WIN
  WSAPoll(&entry, 1, static_cast<INT>(timeout.count()));
#else
  poll(&entry, 1, static_cast<int>(timeout.count()));
#endif
  // nothing to report: whatever happened, the caller's loop reads next
}

void TCPClientBackend::CleanupSocket() {
  CloseSocket(client_socket_);
  client_socket_ = kSocketInvalid;
  is_bind_ = false;
  if (client_session_) {
    client_session_->Close();
    client_session_ = nullptr;
  }
}

TCPServerBackend::TCPServerBackend(std::shared_ptr<InetAddress> bind_address,
                                   const SessionOptions& child_options,
                                   const ServerOptions& server_options)
    : child_options_(child_options),
      server_options_(server_options),
      admission_(server_options),
      bind_address_(bind_address) {}

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

  const bool is_unix = bind_address_->ipv() == InetProtocolVersion::Unix;
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
  if (!is_unix && server_options_.reuse_address) {
#ifdef ZNET_TARGET_WIN
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR | SO_BROADCAST, &option,
               sizeof(option));
#else
    setsockopt(server_socket_, SOL_SOCKET,
               SO_REUSEADDR | SO_REUSEPORT | SO_BROADCAST, &option,
               sizeof(option));
#endif
  }
  if (!SetSocketBlocking(server_socket_, false)) {
    ZNET_LOG_ERROR("Error setting socket to non-blocking mode: {}",
                   GetLastErrorInfo());
    CloseSocket(server_socket_);
    return Result::Failure;
  }
  if (!is_unix) {
    SetTCPNoDelay(server_socket_);
  }
#if ZNET_HAS_AF_UNIX
  if (is_unix) {
    // a stale socket file from a dead listener refuses the bind with
    // EADDRINUSE; taking the path over is what every daemon does
    unlink(static_cast<const InetAddressUnix*>(bind_address_.get())->path());
  }
#endif
  if (bind(server_socket_, bind_address_->handle_ptr(),
           bind_address_->addr_size()) != 0) {
    ZNET_LOG_DEBUG("Failed to bind: {}, {}", bind_address_->readable(),
                   GetLastErrorInfo());
    return Result::CannotBind;
  }
  // read the address back, so a port of 0 resolves to what was assigned. a
  // path has nothing to resolve.
  if (!is_unix) {
    sockaddr_storage local_ss{};
    socklen_t local_len = sizeof(local_ss);
    if (getsockname(server_socket_, reinterpret_cast<sockaddr*>(&local_ss), &local_len) == 0) {
      bind_address_ = InetAddress::from(reinterpret_cast<sockaddr*>(&local_ss));
    } else {
      ZNET_LOG_ERROR("getsockname failed: {}", GetLastErrorInfo());
    }
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
  const int backlog =
      server_options_.backlog > 0 ? server_options_.backlog : SOMAXCONN;
  if (listen(server_socket_, backlog) != 0) {
    ZNET_LOG_DEBUG("Failed to listen connections from: {}, {}",
                   bind_address_->readable(), GetLastErrorInfo());
    return Result::CannotListen;
  }
  is_listening_ = true;
  poll_task_.Run([this]() { PollLoop(); });
  return Result::Success;
}

void TCPServerBackend::StopReceiving() {
  poll_task_.RequestStop();
  poll_task_.Wait();
}

void TCPServerBackend::PollLoop() {
  while (!poll_task_.IsStopRequested()) {
    std::vector<pollfd> fds;
    {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      fds.reserve(polled_.size());
      for (SocketHandle handle : polled_) {
        pollfd entry{};
        entry.fd = handle;
        entry.events = POLLIN;
        fds.push_back(entry);
      }
    }
    if (fds.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
#ifdef ZNET_TARGET_WIN
    const int ready = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 10);
#else
    const int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), 10);
#endif
    if (ready <= 0) {
      continue;
    }
    bool wake = false;
    std::vector<SocketHandle> dead;
    for (const pollfd& entry : fds) {
      if ((entry.revents & POLLNVAL) != 0) {
        // the transport closed the descriptor; stop watching the number
        // before something else in the process reuses it
        dead.push_back(entry.fd);
        continue;
      }
      if ((entry.revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        wake = true;  // data, or a close the worker has to notice
      }
    }
    if (!dead.empty()) {
      std::lock_guard<std::mutex> lock(poll_mutex_);
      polled_.erase(std::remove_if(polled_.begin(), polled_.end(),
                                   [&dead](SocketHandle handle) {
                                     return std::find(dead.begin(), dead.end(),
                                                      handle) != dead.end();
                                   }),
                    polled_.end());
    }
    if (wake && on_data_) {
      on_data_();
      // level-triggered: the bytes stay readable until a worker drains them,
      // so pause rather than re-fire the wake in a tight loop. Short, because
      // this pause is also the floor under back-to-back round trips.
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
}

Result TCPServerBackend::Close() {
  StopReceiving();
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_listening_) {
    return Result::AlreadyStopped;
  }
  // Close the server
  if (!CloseSocket(server_socket_)) {
    ZNET_LOG_DEBUG("Failed to close socket: {}, {}",
                   bind_address_->readable(), GetLastErrorInfo());
  }
#if ZNET_HAS_AF_UNIX
  // the socket file outlives the descriptor; leaving it would make the next
  // bind here depend on the takeover in Bind()
  if (bind_address_ && bind_address_->ipv() == InetProtocolVersion::Unix) {
    unlink(static_cast<const InetAddressUnix*>(bind_address_.get())->path());
  }
#endif
  is_listening_ = false;
  is_bind_ = false;
  return Result::Success;
}

void TCPServerBackend::Update() {

}

std::shared_ptr<PeerSession> TCPServerBackend::Accept() {
  // a refused connection is closed and the next pending one looked at, so a
  // denylisted source cannot stall the accept queue for everyone else
  for (;;) {
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
    if (bind_address_->ipv() != InetProtocolVersion::Unix) {
      SetTCPNoDelay(client_socket);
    }
    std::shared_ptr<InetAddress> remote_address = InetAddress::from(reinterpret_cast<sockaddr*>(&client_address));
    if (remote_address == nullptr) {
      CloseSocket(client_socket);
      continue;
    }
    const AdmissionControl::Verdict verdict = admission_.Admit(*remote_address);
    if (verdict != AdmissionControl::Verdict::Allow) {
      ZNET_LOG_DEBUG("Refused connection from {} ({}).",
                     remote_address->readable(), GetVerdictString(verdict));
      CloseSocket(client_socket);
      continue;
    }
    {
      // watched from here on, so inbound data wakes a worker instead of
      // waiting out its tick
      std::lock_guard<std::mutex> lock(poll_mutex_);
      polled_.push_back(client_socket);
    }
    return std::make_shared<PeerSession>(bind_address_, remote_address,
                                      std::make_unique<TCPTransportLayer>(client_socket, child_options_.common), ConnectionType::TCP,
                                      /*is_initiator=*/false,
                                      /*self_managed=*/false, child_options_);
  }
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
