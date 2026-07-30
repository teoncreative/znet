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

#include "znet/backends/backend.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"

#include <deque>

namespace znet {
namespace backends {

class TCPTransportLayer : public TransportLayer {
 public:
  TCPTransportLayer(SocketHandle socket);
  ~TCPTransportLayer() override;

  std::shared_ptr<Buffer> Receive() override;
  bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) override;

  Result Close(CloseOptions options = {}) override;

  bool IsClosed() override { return is_closed_.load(std::memory_order_acquire); }

  void Update() override;

  void Flush() override;

  void FillMetrics(SessionMetrics& out) const override;

 private:
  std::shared_ptr<Buffer> ReadBuffer();

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

  std::shared_ptr<PeerSession> client_session() override { return client_session_; }

  std::shared_ptr<InetAddress> local_address() override { return local_address_; }

 private:
  void CleanupSocket();
 private:
  SessionOptions options_;
  std::shared_ptr<InetAddress> server_address_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<PeerSession> client_session_;
  bool is_bind_ = false;
  SocketHandle client_socket_ = kSocketInvalid;
};

class TCPServerBackend : public ServerBackend {
 public:
  explicit TCPServerBackend(std::shared_ptr<InetAddress> bind_address,
                            const SessionOptions& child_options = {});
  ~TCPServerBackend() override;
  TCPServerBackend(const TCPServerBackend&) = delete;

  Result Bind() override;
  Result Listen() override;
  Result Close() override;

  void Update() override;

  std::shared_ptr<PeerSession> Accept() override;
  void AcceptAndReject() override;

  bool IsAlive() override;

  std::shared_ptr<InetAddress> bind_address() const override {
    return bind_address_;
  }

 private:
  // serializes Close() against Accept(): the server's loop accepts on
  // server_socket_ while the application may close it from its own thread, and
  // accept() on a descriptor that has been closed and reused would hand back a
  // connection belonging to something else.
  std::mutex mutex_;
  SessionOptions child_options_;
  std::shared_ptr<InetAddress> bind_address_;
  // read by IsAlive() outside mutex_, so they cannot be plain bools
  std::atomic_bool is_bind_{false};
  std::atomic_bool is_listening_{false};
  SocketHandle server_socket_ = kSocketInvalid;
};

}  // namespace backends
}  // namespace znet
#endif  //ZNET_PARENT_TCP_H
