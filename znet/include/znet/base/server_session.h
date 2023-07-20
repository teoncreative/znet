//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "connection_session.h"

namespace znet {
  class ServerSession : public ConnectionSession {
  public:
    ServerSession(Ref<InetAddress> local_address, Ref<InetAddress> remote_address, int client_socket);

    void Process() override;
    void Close() override;

    bool IsAlive() override;

    ZNET_NODISCARD Ref<InetAddress> local_address() const override { return local_address_; }
    ZNET_NODISCARD Ref<InetAddress> remote_address() const override { return remote_address_; }

    void SendPacket(Ref<Packet> packet) override;
    void SendRaw(Ref<Buffer> buffer) override;

  private:
    Ref<InetAddress> local_address_;
    Ref<InetAddress> remote_address_;
    int client_socket_;

    char buffer_[MAX_BUFFER_SIZE]{};
    ssize_t data_size_ = 0;
    bool is_alive_;
  };
}