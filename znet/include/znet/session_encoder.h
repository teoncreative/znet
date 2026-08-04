//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Internal: a private member of Client, public only because that member
// needs the complete type. Nothing here is meant to be called directly.
//
// A thread that encodes one session's queued packets, so the loop putting bytes
// on the wire does not have to stop and do it.
//
// Its own type rather than living on either side: a Client should not own thread
// scheduling for a session it merely holds, and a PeerSession should not grow a
// second thread for something only some of its owners want. A server's workers
// encode inline across many sessions and never build one of these.
//

#ifndef ZNET_SESSION_ENCODER_H_
#define ZNET_SESSION_ENCODER_H_

#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/task.h"
#include "znet/worker_signal.h"

#include <functional>
#include <memory>

namespace znet {

/**
 * @brief Drains a session's outbound queue on a thread of its own.
 *
 * Encoding a message and putting it on the wire are separate costs; running
 * both on one thread makes them serial. This overlaps them, which is worth
 * roughly half the throughput once payloads are large enough that encoding is
 * the expensive half.
 */
class SessionEncoder {
 public:
  SessionEncoder() = default;
  ~SessionEncoder() { Stop(); }

  SessionEncoder(const SessionEncoder&) = delete;
  SessionEncoder& operator=(const SessionEncoder&) = delete;

  /**
   * @brief Takes over encoding for `session` and starts the thread.
   *
   * Installs the session's wake callback, so call it before the session is
   * visible to the application.
   *
   * @param wake_flusher ends the sleep of whoever puts bytes on the wire. Run
   *        after a drain that produced something, and also on the send that
   *        wakes this encoder: a lone message is better encoded by whichever
   *        thread is already awake than handed across one, which is the choice
   *        OutboundQueue's inline-depth rule makes.
   */
  void Start(std::shared_ptr<PeerSession> session,
             std::function<void()> wake_flusher);

  /**
   * @brief Stops and joins, and hands encoding back to the session's loop.
   *
   * Safe to call more than once, and on an encoder that never started.
   */
  void Stop();

 private:
  void Loop();

  std::shared_ptr<PeerSession> session_;
  std::function<void()> wake_flusher_;
  // shared, and never reassigned after construction, because the session's wake
  // callback holds it and a session may outlive this encoder.
  std::shared_ptr<WorkerSignal> signal_{std::make_shared<WorkerSignal>()};
  // a Task rather than a bare thread, so stopping and joining works the way it
  // does everywhere else in the library.
  Task task_;
};

}  // namespace znet


#endif  // ZNET_SESSION_ENCODER_H_
