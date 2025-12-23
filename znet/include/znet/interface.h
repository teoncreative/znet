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

#include "znet/event.h"
#include "znet/packet_handler.h"
#include "znet/precompiled.h"

#include <utility>

namespace znet {

class Interface {
 public:
  Interface() = default;

  virtual ~Interface() = default;

  virtual Result Bind() = 0;

  virtual void Wait() = 0;

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD EventCallbackFn event_callback() const {
    return event_callback_;
  }

 protected:
  EventCallbackFn event_callback_;
};

}  // namespace znet