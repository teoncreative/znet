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
// ZDT's round-trip estimator and congestion window, as two plain state
// machines. Neither owns a socket, a clock or a connection: time arrives as a
// parameter and everything they need about the rest of the transport is passed
// in. That is what makes them testable, which matters here more than usual,
// because congestion control only misbehaves on links that are awkward to
// reproduce.
//

#ifndef ZNET_PARENT_ZDT_CONGESTION_H
#define ZNET_PARENT_ZDT_CONGESTION_H

#include "znet/backends/zdt/zdt_wire.h"
#include "znet/compat.h"
#include "znet/options.h"
#include "znet/precompiled.h"

#include <chrono>

namespace znet {
namespace backends {

/**
 * @brief Jacobson/Karels smoothed round trip, plus the windowed minimum the
 *        congestion signal is measured against.
 */
class ZDTRttEstimator {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  /** @brief Seeds the retransmit timeout before any sample has arrived. */
  void Reset(std::chrono::milliseconds rto) { rto_ = rto; }

  /**
   * @brief Folds in one round-trip measurement.
   *
   * @param sample  measured round trip; negative samples are ignored.
   * @param now     used to age the windowed minimum.
   * @param rto_min lower clamp on the resulting retransmit timeout.
   * @param rto_max upper clamp.
   */
  void OnSample(std::chrono::steady_clock::duration sample, TimePoint now,
                std::chrono::milliseconds rto_min,
                std::chrono::milliseconds rto_max);

  /**
   * @brief Whether the round trip has grown enough above its floor to read as a
   *        queue building rather than jitter.
   *
   * Inline: evaluated per ack and per retransmit scan.
   */
  ZNET_NODISCARD bool IsQueueing() const {
    return has_rtt_ && rtt_min_ms_ > 0.0 &&
           srtt_ms_ > rtt_min_ms_ * kZDTQueueingRttRatio;
  }

  ZNET_NODISCARD bool has_rtt() const { return has_rtt_; }
  ZNET_NODISCARD double srtt_ms() const { return srtt_ms_; }
  ZNET_NODISCARD double rtt_min_ms() const { return rtt_min_ms_; }
  ZNET_NODISCARD std::chrono::milliseconds rto() const { return rto_; }

 private:
  double srtt_ms_ = 0.0;
  double rttvar_ms_ = 0.0;
  // a windowed minimum, not a lifetime one. the whole controller is measured
  // against this, so a stale floor left by a route change would read the new
  // baseline as permanent queueing and pin the window at its lower bound.
  double rtt_min_ms_ = 0.0;
  TimePoint rtt_min_stamp_;
  bool has_rtt_ = false;
  std::chrono::milliseconds rto_{200};
};

/**
 * @brief The congestion window, in datagrams.
 *
 * The congestion signal is queueing delay rather than loss. Reno-style halving
 * on every drop settles at a window of ~1.2/sqrt(loss), six datagrams at 5%,
 * which collapses throughput on a link that is lossy rather than congested. A
 * full queue raises the round trip; a corrupted radio frame does not.
 *
 * Loss events are grouped into epochs so that one burst costs one reduction
 * rather than one per datagram in it. The epoch is defined against the sender's
 * packet sequence, which is why the send-path counter is passed in.
 */
class ZDTCongestionController {
 public:
  /** @brief Ends the current loss epoch once the peer acknowledges something
   *  sent after it opened. */
  void OnAckArrived(WireSeq peer_ack);

  /**
   * @brief Grows the window for newly acknowledged datagrams, or backs off if
   *        the round trip says a queue is building.
   *
   * @param acked_datagrams how many datagrams this ack retired.
   * @param rtt             the queueing signal.
   * @param next_seq        the sender's next packet sequence, which bounds the
   *                        loss epoch this may open.
   * @param cap             ceiling on the window.
   */
  void OnAcked(int acked_datagrams, const ZDTRttEstimator& rtt,
               WireSeq next_seq, int cap);

  /**
   * @brief A retransmit scan found something timed out.
   *
   * Collapses hard only when the round trip says a queue is involved; a plain
   * lossy path gets a gentler reduction, since a timeout there is usually still
   * just loss.
   */
  void OnRetransmitTimeout(const ZDTRttEstimator& rtt, WireSeq next_seq);

  /**
   * @brief Datagrams allowed in flight, clamped to `cap` and to a floor that
   *        always permits forward progress.
   *
   * Inline: evaluated per iteration of the flush loop.
   */
  ZNET_NODISCARD int Window(int cap) const {
    int w = static_cast<int>(cwnd_);
    if (w < 2) {
      w = 2;  // always allow enough in flight to make forward progress
    }
    return w > cap ? cap : w;
  }

  ZNET_NODISCARD double cwnd() const { return cwnd_; }
  ZNET_NODISCARD bool in_loss_recovery() const { return in_loss_recovery_; }

 private:
  double cwnd_ = 10.0;     // initial window, TCP's IW10
  double ssthresh_ = 1e9;  // no threshold until the first loss teaches one
  // suppresses repeated reduction inside one round trip: one loss event should
  // cost one reduction, not one per lost datagram in the same window.
  WireSeq loss_recovery_until_ = 0;
  bool in_loss_recovery_ = false;
};

}  // namespace backends
}  // namespace znet

#endif  // ZNET_PARENT_ZDT_CONGESTION_H
