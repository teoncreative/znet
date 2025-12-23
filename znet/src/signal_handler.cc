//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/signal_handler.h"

#include <utility>

namespace znet {

SignalHandlerFn handler_fn_;

void RegisterSignalHandler(SignalHandlerFn fn, Signal sig) {
  handler_fn_ = std::move(fn);
  signal(sig, [](int sig) {
    if (handler_fn_(static_cast<Signal>(sig))) {
      exit(0);
    }
  });
}

}  // namespace znet