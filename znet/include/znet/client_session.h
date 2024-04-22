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
#include <mutex>

namespace znet {

class ClientSession : public ConnectionSession {
 public:
  ClientSession(Ref<InetAddress> local_address, Ref<InetAddress> remote_address,
                SocketType socket_);

  void Process() override;
  Result Close() override;

  bool IsAlive() override { return is_alive_; }

  void SendPacket(Ref<Packet> packet) override;

  void SendRaw(Ref<Buffer> buffer) override;

 private:
  std::mutex mutex_;
  SocketType socket_;
  char buffer_[MAX_BUFFER_SIZE]{};
  ssize_t data_size_ = 0;
  bool is_alive_;
};

}  // namespace znet