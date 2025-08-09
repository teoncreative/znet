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
#include "znet/peer_session.h"
#include "znet/precompiled.h"

namespace znet {
namespace backends {

class TCPTransportLayer : public TransportLayer {
 public:
  TCPTransportLayer(SocketHandle socket);
  ~TCPTransportLayer();

  std::shared_ptr<Buffer> Receive() override;
  bool Send(std::shared_ptr<Buffer> buffer, SendOptions options) override;

  Result Close() override;

  bool IsClosed() override { return is_closed_; }

 private:
  std::shared_ptr<Buffer> ReadBuffer();

  char data_[ZNET_MAX_BUFFER_SIZE]{};
  int read_offset_ = 0;
  ssize_t data_size_ = 0;
  std::shared_ptr<Buffer> buffer_;
  SocketHandle socket_;
  bool has_more_;
  bool is_closed_ = false;

};

class TCPClientBackend : public ClientBackend {
 public:
  TCPClientBackend(std::shared_ptr<InetAddress> server_address);
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
  std::shared_ptr<InetAddress> server_address_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<PeerSession> client_session_;
  bool is_bind_ = false;
  SocketHandle client_socket_ = -1;
};

class TCPServerBackend : public ServerBackend {
 public:
  TCPServerBackend(std::shared_ptr<InetAddress> bind_address);
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

 private:
  std::mutex mutex_;
  std::shared_ptr<InetAddress> bind_address_;
  bool is_bind_ = false;
  bool is_listening_ = false;
  SocketHandle server_socket_ = -1;
};

}  // namespace backends
}  // namespace znet
#endif  //ZNET_PARENT_TCP_H
