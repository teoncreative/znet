//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_INTERFACE_H_
#define ZNET_INTERFACE_H_

#include "znet/compat.h"
#include "znet/event.h"
#include "znet/packet_handler.h"

#include <utility>

namespace znet {

/**
 * @brief Shared base of everything that binds a socket and reports through
 *        events: Client, Server and the P2P locators.
 */
class Interface {
 public:
  Interface() = default;

  virtual ~Interface() = default;

  virtual Result Bind() = 0;

  virtual void Wait() = 0;

  /**
   * @brief Installs the callback every event is delivered through.
   *
   * Set it before Bind()/Connect()/Listen(): events fire from internal worker
   * threads, and replacing the callback while they run is a data race.
   */
  void SetEventCallback(EventCallbackFn fn) { event_callback_ = std::move(fn); }

  ZNET_NODISCARD const EventCallbackFn& event_callback() const {
    return event_callback_;
  }

 protected:
  EventCallbackFn event_callback_;
};

}  // namespace znet

#endif  // ZNET_INTERFACE_H_
