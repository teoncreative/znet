
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
#include "znet/buffer.h"
#include "znet/close_options.h"
#include "znet/send_options.h"

namespace znet {

class TransportLayer {
 public:
  virtual ~TransportLayer() = default;

  virtual std::shared_ptr<Buffer> Receive() = 0;
  virtual bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) = 0;

  virtual Result Close(CloseOptions options = {}) = 0;

  virtual bool IsClosed() = 0;

  virtual void Update() = 0;

};

}
