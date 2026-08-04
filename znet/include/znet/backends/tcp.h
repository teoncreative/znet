//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_PARENT_TCP_H
#define ZNET_PARENT_TCP_H

#include "znet/admission.h"
#include "znet/backends/backend.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"

#include <deque>

namespace znet {
namespace backends {

class TCPTransportLayer : public TransportLayer {
 public:
  // `common` carries the keepalive knobs; the default matches CommonOptions
  // so call sites without a SessionOptions in hand behave like a default
  // session
  TCPTransportLayer(SocketHandle socket,
                    CommonOptions common = CommonOptions());
  ~TCPTransportLayer() override;

  std::shared_ptr<Buffer> Receive() override;
  bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) override;

  Result Close(CloseOptions options = {}) override;

  bool IsClosed() override { return is_closed_.load(std::memory_order_acquire); }

  void Update() override;

  void Flush() override;

  void FillMetrics(SessionMetrics& out) const override;

 private:
  // control frames ride inside the stream as a zero length prefix followed by
  // one of these. A data frame's payload is never empty (Send refuses them),
  // so a zero length is unambiguous.
  static constexpr uint8_t kControlPing = 1;
  static constexpr uint8_t kControlPong = 2;

  std::shared_ptr<Buffer> ReadBuffer();

  void HandleControl(uint8_t type);

  /** @brief Writes one control frame; a failure is left to the idle timer. */
  void SendControl(uint8_t type);

  /** @brief Writes a whole framed message, looping over partial sends. */
  bool WriteAll(Buffer& buffer);

  char data_[ZNET_MAX_BUFFER_SIZE]{};
  ssize_t read_offset_ = 0;
  ssize_t data_size_ = 0;
  std::shared_ptr<Buffer> buffer_;
  SocketHandle socket_;
  // read by IsClosed() from whichever thread owns the application, written by
  // Close() from the same, so it cannot be a plain bool
  std::atomic_bool is_closed_{false};
  std::chrono::milliseconds keepalive_interval_;
  std::chrono::milliseconds idle_timeout_;
  // touched only by the worker driving Receive()/Update()
  std::chrono::steady_clock::time_point last_recv_;
  // serializes socket writes: data frames go out under the session's encode
  // claim while pings and pongs come from the worker, and interleaved send()s
  // would splice two frames together. last_send_ is guarded by it too, since
  // both paths stamp it.
  std::mutex write_mutex_;
  std::chrono::steady_clock::time_point last_send_;
#if ZNET_ENABLE_METRICS
  SessionMetrics metrics_;
#endif

};

class TCPClientBackend : public ClientBackend {
 public:
  explicit TCPClientBackend(std::shared_ptr<InetAddress> server_address,
                            const SessionOptions& options = {});
  ~TCPClientBackend() override;
  TCPClientBackend(const TCPClientBackend&) = delete;

  Result Bind() override;
  Result Bind(const std::string& ip, PortNumber port) override;

  Result Connect() override;
  Result Close() override;

  void Update() override;

  bool IsAlive() override;

  void WaitReadable(std::chrono::milliseconds timeout) override;

  std::shared_ptr<PeerSession> client_session() override { return client_session_; }

  std::shared_ptr<InetAddress> local_address() override { return local_address_; }

  void ReleaseSession() override {
    if (client_session_ && !client_session_->IsAlive()) {
      client_session_ = nullptr;
    }
  }

 private:
  void CleanupSocket();
 private:
  SessionOptions options_;
  std::shared_ptr<InetAddress> server_address_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<PeerSession> client_session_;
  bool is_bind_ = false;
  SocketHandle client_socket_ = kSocketInvalid;
  // non-owning copy kept after the transport takes the descriptor, so
  // WaitReadable can poll it; the transport closes it, never this
  SocketHandle wait_socket_ = kSocketInvalid;
};

class TCPServerBackend : public ServerBackend {
 public:
  explicit TCPServerBackend(std::shared_ptr<InetAddress> bind_address,
                            const SessionOptions& child_options = {},
                            const ServerOptions& server_options = {});
  ~TCPServerBackend() override;
  TCPServerBackend(const TCPServerBackend&) = delete;

  Result Bind() override;
  Result Listen() override;
  Result Close() override;

  void Update() override;

  std::shared_ptr<PeerSession> Accept() override;
  void AcceptAndReject() override;

  bool IsAlive() override;

  void SetWakeCallback(std::function<void()> on_data) override {
    on_data_ = std::move(on_data);
  }

  void StopReceiving() override;

  std::shared_ptr<InetAddress> bind_address() const override {
    return bind_address_;
  }

 private:
  /**
   * @brief Watches every accepted socket and fires the wake callback when
   *        one turns readable.
   *
   * Without it, inbound TCP data sat until a worker's next tick: an 8 ms
   * round-trip floor at the default 120 tps, three orders of magnitude above
   * the socket's own latency.
   */
  void PollLoop();
  // serializes Close() against Accept(): the server's loop accepts on
  // server_socket_ while the application may close it from its own thread, and
  // accept() on a descriptor that has been closed and reused would hand back a
  // connection belonging to something else.
  std::mutex mutex_;
  SessionOptions child_options_;
  ServerOptions server_options_;
  // touched only from Accept(), which one thread drives
  AdmissionControl admission_;
  std::shared_ptr<InetAddress> bind_address_;
  // read by IsAlive() outside mutex_, so they cannot be plain bools
  std::atomic_bool is_bind_{false};
  std::atomic_bool is_listening_{false};
  SocketHandle server_socket_ = kSocketInvalid;
  std::function<void()> on_data_;
  Task poll_task_;
  // accepted sockets under watch; the poll thread prunes entries whose
  // descriptors have been closed by their transports
  std::mutex poll_mutex_;
  std::vector<SocketHandle> polled_;
};

}  // namespace backends
}  // namespace znet
#endif  //ZNET_PARENT_TCP_H
