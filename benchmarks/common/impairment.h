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
// Tells a benchmark what the link underneath it looks like, so it can size its
// workloads for that link and label the output. It does not damage anything
// itself: netem does, in an unprivileged network namespace.
//
//   unshare -rn sh -c '
//     ip link set lo mtu 1500
//     ip link set lo up
//     tc qdisc add dev lo root netem delay 25ms loss 5%
//     ZNET_BENCH_IMPAIR="delay=25,loss=5" ./znet-bench'
//
// No root, and it does not touch the host's networking. netem is used rather
// than a userspace forwarder because it sits below the socket and therefore
// impairs TCP as well: dropping bytes out of a proxied TCP stream is
// corruption, not packet loss, since TCP retransmits below the socket API.
// A UDP forwarder can only ever impair the datagram transports, which would
// leave the TCP rows silently unimpaired in the middle of an impaired table.
//
// The values here have to match what netem was given, because nothing can read
// them back. They are used for two things:
//
//   - scaling the workloads. The defaults assume a microsecond round trip; at
//     a 300 ms RTT the stock 2,000 ping-pongs is ten minutes and a
//     one-message-per-datagram throughput case runs past the harness deadline.
//   - labelling the output, so an impaired table is never mistaken for a clean
//     one.
//
// Read `delay` and `jitter` as ONE WAY: a round trip pays both twice.
//

#ifndef ZNET_BENCH_IMPAIRMENT_H
#define ZNET_BENCH_IMPAIRMENT_H

#include "common/harness.h"  // Workload, and the defaults this scales

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace bench {

/** @brief What netem was told to do to the link. One-way delay and jitter. */
struct Impairment {
  double loss_percent = 0.0;
  int delay_ms = 0;
  int jitter_ms = 0;
  double dup_percent = 0.0;

  /** @brief True when this run is impaired at all. */
  bool enabled() const {
    return loss_percent > 0.0 || delay_ms > 0 || jitter_ms > 0 ||
           dup_percent > 0.0;
  }

  /** @brief Parses ZNET_BENCH_IMPAIR, e.g. "loss=5,delay=25,jitter=5". */
  static Impairment FromEnv() {
    Impairment cfg;
    const char* spec = std::getenv("ZNET_BENCH_IMPAIR");
    if (!spec) {
      return cfg;
    }
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
      size_t comma = s.find(',', pos);
      if (comma == std::string::npos) {
        comma = s.size();
      }
      std::string item = s.substr(pos, comma - pos);
      size_t eq = item.find('=');
      if (eq != std::string::npos) {
        std::string key = item.substr(0, eq);
        std::string value = item.substr(eq + 1);
        if (key == "loss") {
          cfg.loss_percent = std::atof(value.c_str());
        } else if (key == "delay") {
          cfg.delay_ms = std::atoi(value.c_str());
        } else if (key == "jitter") {
          cfg.jitter_ms = std::atoi(value.c_str());
        } else if (key == "dup") {
          cfg.dup_percent = std::atof(value.c_str());
        }
      }
      pos = comma + 1;
    }
    return cfg;
  }

  std::string Describe() const {
    if (!enabled()) {
      return "none";
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "%.3g%% loss, %d ms +/- %d ms one-way delay, %.3g%% dup",
                  loss_percent, delay_ms, jitter_ms, dup_percent);
    return std::string(buf);
  }
};

// The scaled counts make the sample noisier, and the absolute numbers are not
// comparable with the clean table. Compare libraries against each other under
// the same impairment, not against their own unimpaired rows.

/** @brief Throughput workloads with counts scaled for the configured delay. */
inline std::vector<Workload> ImpairedThroughputWorkloads(const Impairment& cfg) {
  std::vector<Workload> workloads = DefaultThroughputWorkloads();
  const double rtt_ms = 2.0 * static_cast<double>(cfg.delay_ms);
  if (!cfg.enabled() || rtt_ms < 1.0) {
    return workloads;  // loss only; the stock counts are still reachable
  }
  const double scale = 20.0 / rtt_ms;
  if (scale >= 1.0) {
    return workloads;
  }
  for (Workload& w : workloads) {
    uint32_t scaled =
        static_cast<uint32_t>(static_cast<double>(w.messages) * scale);
    w.messages = scaled < 200u ? 200u : scaled;
  }
  return workloads;
}

/** @brief Ping-pong workload with a sample count scaled for the delay. */
inline Workload ImpairedLatencyWorkload(const Impairment& cfg) {
  Workload w = DefaultLatencyWorkload();
  const double rtt_ms = 2.0 * static_cast<double>(cfg.delay_ms);
  if (!cfg.enabled() || rtt_ms < 1.0) {
    return w;
  }
  uint32_t samples = static_cast<uint32_t>(20000.0 / rtt_ms);
  if (samples < 50u) {
    samples = 50u;
  }
  if (samples < w.messages) {
    w.messages = samples;
  }
  return w;
}

/** @brief Prints the active impairment once, at the top of a run. */
inline void NoteImpairment(const Impairment& cfg) {
  if (cfg.enabled()) {
    std::printf("  impairment: %s (applied by netem, not by the benchmark)\n",
                cfg.Describe().c_str());
  }
}

}  // namespace bench

#endif  // ZNET_BENCH_IMPAIRMENT_H
