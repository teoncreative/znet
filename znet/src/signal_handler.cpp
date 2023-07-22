//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <utility>

#include "znet/base/signal_handler.h"

namespace znet {

SignalHandlerFn handler_fn_;

void RegisterSignalHandler(SignalHandlerFn fn) {
  handler_fn_ = std::move(fn);
  signal(SIGINT, [](int sig) {
    if (handler_fn_(static_cast<Signal>(sig))) {
      exit(0);
    }
  });
}
}