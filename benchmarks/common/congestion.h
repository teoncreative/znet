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
// Congestion pool: a fixed-duration bulk transfer with a small probe running
// through it. Reports ramp (time to 90% of own peak), steady rate (second half
// only), and loaded latency (probe RTT while the link is saturated). Compare
// `loaded-lat` with the idle `latency` row to read the standing queue.
//
// Mostly meaningful under netem: on clean loopback no window binds and the
// loaded latency is the idle latency. Duration-based, so impaired runs need no
// workload scaling. The `sep` column says how the probe was kept off the bulk
// stream (channel / conn / none); rows with different sep are not a controller
// comparison.
//

#ifndef ZNET_BENCH_CONGESTION_H
#define ZNET_BENCH_CONGESTION_H

#include "common/harness.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace bench {

/** @brief One congestion case. probe_bytes must differ from bulk_bytes: size
 *         is how the echoing side tells the two apart. */
struct CongestionCase {
  const char* name;
  size_t bulk_bytes;
  size_t probe_bytes;
  std::chrono::milliseconds duration;
  std::chrono::milliseconds bucket;       // ramp sampling interval
  std::chrono::milliseconds probe_every;  // gap between echo and next probe
};

// Same near-MTU / fragmenting split as the throughput pool.
inline std::vector<CongestionCase> DefaultCongestionCases() {
  return {
      {"1KB", 1024, 64, std::chrono::milliseconds(10000),
       std::chrono::milliseconds(100), std::chrono::milliseconds(20)},
      {"8KB", 8192, 64, std::chrono::milliseconds(10000),
       std::chrono::milliseconds(100), std::chrono::milliseconds(20)},
  };
}

/** @brief What one pump() call delivered, split by stream. */
struct PumpCounts {
  uint32_t bulk = 0;
  uint32_t probes = 0;
};

struct CongestionResult {
  uint32_t bulk_delivered = 0;
  Clock::duration elapsed{};
  double cpu_seconds = 0.0;
  std::vector<uint32_t> buckets;  // bulk deliveries per sampling bucket
  std::vector<double> probe_us;   // loaded round trips
  uint32_t probes_lost = 0;
};

// Drives one case. Bulk keeps the same 4096 backlog bound as the throughput
// pool; only one probe is ever outstanding.
template <typename SendBulk, typename SendProbe, typename Pump>
CongestionResult RunCongestionLoop(const CongestionCase& c, SendBulk send_bulk,
                                   SendProbe send_probe, Pump pump) {
  constexpr uint32_t kMaxBacklog = 4096;
  constexpr auto kProbeTimeout = std::chrono::seconds(2);

  CongestionResult out;
  const double cpu_start = ProcessCPUSeconds();
  const auto start = Clock::now();
  const auto finish = start + c.duration;
  auto next_bucket = start + c.bucket;
  auto next_probe = start;
  uint32_t bulk_sent = 0;
  uint32_t bulk_done = 0;
  uint32_t bucket_base = 0;
  bool probe_outstanding = false;
  Clock::time_point probe_sent_at{};

  while (Clock::now() < finish) {
    while (bulk_sent - bulk_done < kMaxBacklog) {
      if (!send_bulk()) {
        break;
      }
      bulk_sent++;
    }

    if (!probe_outstanding && Clock::now() >= next_probe) {
      probe_sent_at = Clock::now();
      if (send_probe()) {
        probe_outstanding = true;
      }
    }

    PumpCounts got = pump();
    bulk_done += got.bulk;
    if (probe_outstanding) {
      const auto now = Clock::now();
      if (got.probes > 0) {
        out.probe_us.push_back(
            std::chrono::duration<double, std::micro>(now - probe_sent_at)
                .count());
        probe_outstanding = false;
        next_probe = now + c.probe_every;
      } else if (now - probe_sent_at > kProbeTimeout) {
        // a probe that never returns is itself a result
        out.probes_lost++;
        probe_outstanding = false;
        next_probe = now + c.probe_every;
      }
    }

    // catch up, so a stalled pump leaves empty buckets rather than one fat one
    while (Clock::now() >= next_bucket) {
      out.buckets.push_back(bulk_done - bucket_base);
      bucket_base = bulk_done;
      next_bucket += c.bucket;
    }
  }

  out.bulk_delivered = bulk_done;
  out.elapsed = Clock::now() - start;
  out.cpu_seconds = ProcessCPUSeconds() - cpu_start;
  return out;
}

struct RampStats {
  double peak_msgs_per_s = 0.0;
  double steady_msgs_per_s = 0.0;  // second half only, excludes slow-start
  double ramp_ms = 0.0;
  bool ramped = false;
};

inline RampStats SummarizeRamp(const CongestionResult& r,
                               std::chrono::milliseconds bucket) {
  RampStats out;
  const double bucket_s = std::chrono::duration<double>(bucket).count();
  if (r.buckets.empty() || bucket_s <= 0.0) {
    return out;
  }

  uint32_t peak = 0;
  for (uint32_t b : r.buckets) {
    peak = std::max(peak, b);
  }
  out.peak_msgs_per_s = static_cast<double>(peak) / bucket_s;

  const double threshold = 0.9 * static_cast<double>(peak);
  for (size_t i = 0; i < r.buckets.size(); i++) {
    if (static_cast<double>(r.buckets[i]) >= threshold) {
      out.ramp_ms =
          std::chrono::duration<double, std::milli>(bucket).count() *
          static_cast<double>(i);
      out.ramped = true;
      break;
    }
  }

  const size_t half = r.buckets.size() / 2;
  uint64_t sum = 0;
  for (size_t i = half; i < r.buckets.size(); i++) {
    sum += r.buckets[i];
  }
  const size_t n = r.buckets.size() - half;
  if (n > 0) {
    out.steady_msgs_per_s =
        static_cast<double>(sum) / (static_cast<double>(n) * bucket_s);
  }
  return out;
}

// Both rows for one case: the median rep by steady rate (loaded-lat comes from
// the same rep, so the pair is coherent). CSV gets every rep.
inline void ReportCongestionCase(const char* library, const char* transport,
                                 const CongestionCase& c,
                                 const std::vector<CongestionResult>& reps,
                                 const char* separation) {
  if (reps.empty()) {
    return;
  }
  std::vector<double> steadies;
  for (size_t i = 0; i < reps.size(); i++) {
    const RampStats ramp = SummarizeRamp(reps[i], c.bucket);
    steadies.push_back(ramp.steady_msgs_per_s);
    const double seconds =
        std::chrono::duration<double>(reps[i].elapsed).count();
    Percentiles p(reps[i].probe_us);
    CsvRow row;
    row.kind = "congestion";
    row.library = library;
    row.transport = transport;
    row.case_name = c.name;
    row.rep = static_cast<int>(i + 1);
    row.delivered = reps[i].bulk_delivered;
    row.seconds = seconds;
    row.msg_per_s =
        seconds > 0 ? static_cast<double>(reps[i].bulk_delivered) / seconds : 0;
    row.mib_per_s = seconds > 0
                        ? (static_cast<double>(reps[i].bulk_delivered) *
                           static_cast<double>(c.bulk_bytes)) /
                              seconds / (1024.0 * 1024.0)
                        : 0;
    row.cpu_us_per_msg = reps[i].bulk_delivered > 0
                             ? reps[i].cpu_seconds * 1e6 /
                                   static_cast<double>(reps[i].bulk_delivered)
                             : NAN;
    row.rtt_count = static_cast<double>(p.count());
    row.mean_us = p.Mean();
    row.p50_us = p.At(0.50);
    row.p95_us = p.At(0.95);
    row.p99_us = p.At(0.99);
    row.probes_lost = reps[i].probes_lost;
    row.ramp_ms = ramp.ramped ? ramp.ramp_ms : NAN;
    row.steady_msg_per_s = ramp.steady_msgs_per_s;
    row.peak_msg_per_s = ramp.peak_msgs_per_s;
    row.separation = separation;
    EmitCsv(row);
  }

  // median rep by steady rate
  std::vector<size_t> order(reps.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&steadies](size_t a, size_t b) {
    return steadies[a] < steadies[b];
  });
  const CongestionResult& mid = reps[order[order.size() / 2]];

  const RampStats ramp = SummarizeRamp(mid, c.bucket);
  const double seconds = std::chrono::duration<double>(mid.elapsed).count();
  const double mib = seconds > 0 ? (static_cast<double>(mid.bulk_delivered) *
                                    static_cast<double>(c.bulk_bytes)) /
                                       seconds / (1024.0 * 1024.0)
                                 : 0.0;
  char ramp_text[16];
  if (ramp.ramped) {
    std::snprintf(ramp_text, sizeof(ramp_text), "%.0f ms", ramp.ramp_ms);
  } else {
    std::snprintf(ramp_text, sizeof(ramp_text), "n/a");
  }
  std::printf("%-10s %-6s congestion %-6s  %8u msgs  %8.1f MiB/s  steady %10.0f msg/s  peak %10.0f  ramp %8s",
              library, transport, c.name, mid.bulk_delivered, mib,
              ramp.steady_msgs_per_s, ramp.peak_msgs_per_s, ramp_text);
  if (reps.size() > 1) {
    std::printf("  [%zu reps: %.0f..%.0f steady]", reps.size(),
                steadies[order.front()], steadies[order.back()]);
  }
  std::printf("\n");

  Percentiles p(mid.probe_us);
  std::printf("%-10s %-6s loaded-lat %-6s  %8zu rtt   mean %7.1f us  p50 %7.1f  p95 %7.1f  p99 %7.1f  lost %4u  sep %s\n",
              library, transport, c.name, p.count(), p.Mean(), p.At(0.50),
              p.At(0.95), p.At(0.99), mid.probes_lost, separation);
}

}  // namespace bench

#endif  // ZNET_BENCH_CONGESTION_H
