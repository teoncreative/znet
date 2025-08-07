
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
#include "znet/send_options.h"
#include "logger.h"
#include "buffer.h"


namespace znet {

class TransportLayer {
 public:
  virtual ~TransportLayer() = default;

  virtual std::shared_ptr<Buffer> Receive() = 0;
  virtual bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) = 0;

  virtual Result Close() = 0;

  virtual bool IsClosed() = 0;

};

}
