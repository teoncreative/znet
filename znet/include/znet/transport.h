
//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "base/packet.h"
#include "logger.h"
#include "buffer.h"


namespace znet {

class PeerSession;

class TransportLayer {
  public:
  TransportLayer(PeerSession& session, SocketType socket);
  ~TransportLayer();

  Ref<Buffer> Receive();
  void Send(Ref<Buffer> buffer);

 private:
  Ref<Buffer> ReadBuffer();

  PeerSession& session_;
  char data_[MAX_BUFFER_SIZE]{};
  ssize_t data_size_ = 0;
  Ref<Buffer> buffer_;
  SocketType socket_;

};

}
