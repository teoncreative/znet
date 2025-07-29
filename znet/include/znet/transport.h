
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

#include "znet/precompiled.h"
#include "znet/base/packet.h"
#include "logger.h"
#include "buffer.h"


namespace znet {

class PeerSession;

class TransportLayer {
  public:
  TransportLayer(PeerSession& session, SocketHandle socket);
  ~TransportLayer();

  std::shared_ptr<Buffer> Receive();
  bool Send(std::shared_ptr<Buffer> buffer);

 private:
  std::shared_ptr<Buffer> ReadBuffer();

  PeerSession& session_;
  char data_[ZNET_MAX_BUFFER_SIZE]{};
  int read_offset_ = 0;
  ssize_t data_size_ = 0;
  std::shared_ptr<Buffer> buffer_;
  SocketHandle socket_;
  bool has_more_;

};

}
