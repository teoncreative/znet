//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/backends/zdt/zdt_congestion.h"

#include <cmath>

namespace znet {
namespace backends {

void ZDTRttEstimator::OnSample(std::chrono::steady_clock::duration sample,
                               TimePoint now,
                               std::chrono::milliseconds rto_min,
                               std::chrono::milliseconds rto_max) {
  const double ms = std::chrono::duration<double, std::milli>(sample).count();
  if (ms < 0.0) {
    return;
  }
  if (!has_rtt_) {
    has_rtt_ = true;
    srtt_ms_ = ms;
    rttvar_ms_ = ms / 2.0;
    rtt_min_ms_ = ms;
    rtt_min_stamp_ = now;
  } else {
    const bool stale =
        now - rtt_min_stamp_ > std::chrono::milliseconds(kZDTRttMinWindowMs);
    if (ms < rtt_min_ms_ || rtt_min_ms_ <= 0.0 || stale) {
      rtt_min_ms_ = ms;
      rtt_min_stamp_ = now;
    }
    rttvar_ms_ = 0.75 * rttvar_ms_ + 0.25 * std::fabs(srtt_ms_ - ms);
    srtt_ms_ = 0.875 * srtt_ms_ + 0.125 * ms;
  }
  const auto rto = std::chrono::milliseconds(
      static_cast<long long>(srtt_ms_ + 4.0 * rttvar_ms_));
  rto_ = compat::Clamp(rto, rto_min, rto_max);
}

// leaving recovery on the first ack of something sent after the loss is what
// keeps one burst from reducing the window more than once
void ZDTCongestionController::OnAckArrived(WireSeq peer_ack) {
  if (in_loss_recovery_ && !SeqLess(peer_ack, loss_recovery_until_)) {
    in_loss_recovery_ = false;
  }
}

void ZDTCongestionController::OnAcked(int acked_datagrams,
                                      const ZDTRttEstimator& rtt,
                                      WireSeq next_seq, int cap) {
  if (acked_datagrams <= 0) {
    return;
  }
  if (rtt.IsQueueing() && cwnd_ > 4.0) {
    if (!in_loss_recovery_) {
      in_loss_recovery_ = true;
      loss_recovery_until_ = next_seq;
      cwnd_ *= kZDTQueueingBackoff;
      ssthresh_ = cwnd_;
    }
    return;
  }
  if (cwnd_ < ssthresh_) {
    cwnd_ += static_cast<double>(acked_datagrams);
  } else {
    cwnd_ += static_cast<double>(acked_datagrams) / cwnd_;
  }
  const double cap_d = static_cast<double>(cap);
  if (cwnd_ > cap_d) {
    cwnd_ = cap_d;
  }
}

void ZDTCongestionController::OnRetransmitTimeout(const ZDTRttEstimator& rtt,
                                                  WireSeq next_seq) {
  if (rtt.IsQueueing()) {
    // guarded like the delay path: a burst of timeouts is one event, and
    // collapsing once per message in it would take several round trips of
    // slow start to undo.
    if (!in_loss_recovery_) {
      ssthresh_ = cwnd_ / 2.0;
      if (ssthresh_ < 2.0) {
        ssthresh_ = 2.0;
      }
      cwnd_ = 2.0;
      in_loss_recovery_ = true;
      loss_recovery_until_ = next_seq;
    }
  } else {
    cwnd_ *= 0.9;
    if (cwnd_ < 8.0) {
      cwnd_ = 8.0;
    }
  }
}

}  // namespace backends
}  // namespace znet
