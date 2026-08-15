
//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_TRANSPORT_H_
#define ZNET_TRANSPORT_H_

#include "znet/buffer.h"
#include "znet/close_options.h"
#include "znet/compat.h"
#include "znet/metrics.h"
#include "znet/send_options.h"

namespace znet {

/**
 * @brief Moves encoded bytes between a session and the wire.
 *
 * @par Threading
 * Every method belongs to the worker driving the owning session's Process(), so
 * a transport needs no locking of its own. The exceptions are Close(), callable
 * from the application's thread, and ZDT's OnDatagram(), called by its receive
 * thread; both say so at their declarations.
 */
class TransportLayer {
 public:
  virtual ~TransportLayer() = default;

  /** @brief The next complete inbound message, or null when none is ready.
   * Worker thread only. */
  virtual std::shared_ptr<Buffer> Receive() = 0;

  /** @brief Hands one encoded message to the transport. Worker thread only. */
  virtual bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) = 0;

  /**
   * @brief Which independently-ordered stream `options` selects. Defaults to a
   *        single stream, correct for one ordered pipe.
   *
   * Messages in one stream arrive in a defined order relative to each other;
   * messages in different streams do not, and either may stall while the other
   * flows. Only the transport knows which is which, so it answers rather than
   * the session reading SendOptions itself. The session crypto needs it: a
   * sequence, and the replay window checking it, mean nothing outside one
   * domain.
   */
  virtual uint8_t OrderingDomain(const SendOptions& options) const {
    (void)options;
    return 0;
  }

  /** @brief Shuts the transport down. Callable from any thread. */
  virtual Result Close(CloseOptions options = {}) = 0;

  /** @brief Whether Close() ran or the peer's close/loss was noticed. Safe
   * from any thread. */
  virtual bool IsClosed() const = 0;

  /** @brief One tick of protocol upkeep: receive, timers, retransmits.
   * Worker thread only. */
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
};

}

#endif  // ZNET_TRANSPORT_H_
