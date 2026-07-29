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

  struct QueuedPacket {
    std::shared_ptr<Buffer> buffer;
    SendOptions options;
  };

  bool SendInternal(std::shared_ptr<Buffer> buffer, SendOptions options);

  char data_[ZNET_MAX_BUFFER_SIZE]{};
  ssize_t read_offset_ = 0;
  ssize_t data_size_ = 0;
  std::shared_ptr<Buffer> buffer_;
  SocketHandle socket_;
  // read by IsClosed() from whichever thread owns the application, written by
  // Close() from the same, so it cannot be a plain bool
  std::atomic_bool is_closed_{false};
  // Send() may be called from any thread, Update() only from the session's
  // worker, so the queue itself needs a lock. It is never held across a socket
  // write: Update() swaps the queue out and drains the copy.
  mutable std::mutex outbound_mutex_;
  std::deque<QueuedPacket> outbound_;
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

  std::mutex& mutex() override { return mutex_; }

  std::shared_ptr<PeerSession> client_session() override { return client_session_; }

  std::shared_ptr<InetAddress> local_address() override { return local_address_; }

 private:
  void CleanupSocket();
 private:
  std::mutex mutex_;
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

  std::mutex& mutex() override { return mutex_; }

  std::shared_ptr<InetAddress> bind_address() const override {
    return bind_address_;
  }

 private:
  std::mutex mutex_;
  SessionOptions child_options_;
  std::shared_ptr<InetAddress> bind_address_;
  // IsAlive() reads these from the main loop while Close() writes them from
  // whichever thread stops the server, and that read is outside mutex_, which
  // the main loop holds across a whole tick
  std::atomic_bool is_bind_{false};
  std::atomic_bool is_listening_{false};
  SocketHandle server_socket_ = kSocketInvalid;
};

}  // namespace backends
}  // namespace znet
#endif  //ZNET_PARENT_TCP_H
