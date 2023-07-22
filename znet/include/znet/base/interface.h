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

#include <utility>

#include "../event/event.h"
#include "packet_handler.h"

namespace znet {

class Interface {
 public:
  Interface() {}

  virtual ~Interface() {}

  virtual void Bind() = 0;

  void SetEventCallback(EventCallbackFn fn) { event_callback_ = fn; }

  EventCallbackFn event_callback() const { return event_callback_; }

 private:
  EventCallbackFn event_callback_;
};

}  // namespace znet