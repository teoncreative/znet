
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
#include "znet/metrics.h"
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

  /**
   * @brief Pushes everything Send() has queued out to the wire.
   *
   * Unlike Update() this skips the per-tick protocol work (timers,
   * retransmits, reassembly pruning). The session calls it after dispatching
   * received messages so a handler's reply leaves in the same tick.
   */
  virtual void Flush() = 0;

  /**
   * @brief Copies this transport's counters into `out`.
   *
   * Fields it does not track are left alone. Called on demand, never on the
   * send or receive path.
   *
   * @param out Destination, which the caller may have pre-filled.
   */
  virtual void FillMetrics(SessionMetrics& out) const { (void)out; }

  /**
   * @brief Registers a callback that ends the owning worker's tick sleep.
   *
   * Send() only queues, and the worker is what puts bytes on the wire, so
   * without this an outbound message on an otherwise idle session waits for
   * the next tick. Set once when the session is handed to a worker and never
   * replaced, so it needs no synchronisation; the callback owns whatever it
   * signals, so it stays valid after the server is gone.
   */
  void SetWakeCallback(std::function<void()> wake) { wake_ = std::move(wake); }

 protected:
  /** @brief Called when a Send() finds the outbound queue empty. */
  void WakeWorker() const {
    if (wake_) {
      wake_();
    }
  }

 private:
  std::function<void()> wake_;
};

}
