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
// Shared measurement and reporting for the benchmarks, so every library is
// scored the same way. Results print as one row per case; run several binaries
// and the rows line up into a comparison table.
//
// Env knobs (all optional):
//   ZNET_BENCH_REPS=N        run each case N times, report the median rep
//   ZNET_BENCH_CSV=path      append machine-readable rows (local diffing only;
//                            CI runners are too noisy for regression numbers)
//   ZNET_BENCH_PAYLOAD=kind  binary (default) | snapshot | text
//

#ifndef ZNET_BENCH_HARNESS_H
#define ZNET_BENCH_HARNESS_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace bench {

using Clock = std::chrono::steady_clock;

// The same workload is run against every library so the numbers are comparable.
struct Workload {
  const char* name;
  size_t payload_bytes;
  uint32_t messages;
};

// Small / near-MTU / large enough to force fragmentation.
inline std::vector<Workload> DefaultThroughputWorkloads() {
  return {
      {"64B", 64, 50000},
      {"1KB", 1024, 20000},
      {"8KB", 8192, 5000},
  };
}

inline Workload DefaultLatencyWorkload() {
  return {"64B ping-pong", 64, 2000};
}

// How compressible the payload is. Measured separately per kind because
// compression is worth wildly different amounts per traffic type, and a
// repeated-byte payload would describe the payload rather than the transport.
enum class PayloadKind {
  Binary,    // no exploitable redundancy: encrypted blobs, packed binary, media
  Snapshot,  // game entity state, the shape most realtime traffic has
  Text,      // chat, JSON, telemetry; the best case for compression
};

inline const char* PayloadKindName(PayloadKind kind) {
  switch (kind) {
    case PayloadKind::Binary:
      return "binary";
    case PayloadKind::Snapshot:
      return "snapshot";
    case PayloadKind::Text:
      return "text";
  }
  return "?";
}

// ZNET_BENCH_PAYLOAD, parsed once. Unknown values warn and fall back to binary.
inline PayloadKind PayloadKindFromEnv() {
  static PayloadKind kind = []() {
    const char* s = std::getenv("ZNET_BENCH_PAYLOAD");
    if (!s) {
      return PayloadKind::Binary;
    }
    std::string v(s);
    if (v == "binary") {
      return PayloadKind::Binary;
    }
    if (v == "snapshot") {
      return PayloadKind::Snapshot;
    }
    if (v == "text") {
      return PayloadKind::Text;
    }
    std::fprintf(stderr,
                 "ZNET_BENCH_PAYLOAD: unknown kind '%s', using binary\n", s);
    return PayloadKind::Binary;
  }();
  return kind;
}

// Deterministic, so runs stay comparable.
inline std::string MakePayload(size_t bytes, PayloadKind kind) {
  std::string out;
  out.reserve(bytes);
  uint32_t state = 0x9E3779B9u;
  auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };

  switch (kind) {
    case PayloadKind::Binary:
      while (out.size() < bytes) {
        out.push_back(static_cast<char>(next() & 0xFFu));
      }
      break;

    case PayloadKind::Snapshot: {
      // {id, position[3], velocity[3], flags}: repeating float exponents,
      // random mantissas, small ids. Compresses a little, not a lot.
      uint16_t id = 1;
      while (out.size() < bytes) {
        auto put = [&out](const void* p, size_t n) {
          out.append(static_cast<const char*>(p), n);
        };
        put(&id, sizeof(id));
        for (int axis = 0; axis < 3; axis++) {
          float position =
              static_cast<float>(next() % 4096u) / 16.0f;  // 0..256 m
          put(&position, sizeof(position));
        }
        for (int axis = 0; axis < 3; axis++) {
          float velocity =
              static_cast<float>(next() % 2048u) / 256.0f - 4.0f;  // +/-4 m/s
          put(&velocity, sizeof(velocity));
        }
        uint8_t flags = static_cast<uint8_t>(next() & 0x0Fu);
        put(&flags, sizeof(flags));
        id++;
      }
      break;
    }

    case PayloadKind::Text: {
      static const char* kWords[] = {
          "the",   "quick",   "brown", "fox",   "jumps",  "over",  "lazy",
          "dog",   "lorem",   "ipsum", "dolor", "sit",    "amet",  "consectetur",
          "adipiscing", "elit", "sed",  "do",    "eiusmod", "tempor"};
      constexpr size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
      while (out.size() < bytes) {
        out.append(kWords[next() % kWordCount]);
        out.push_back(' ');
      }
      break;
    }
  }
  out.resize(bytes);
  return out;
}

// Default kind is binary (incompressible), overridable via ZNET_BENCH_PAYLOAD.
inline std::string MakePayload(size_t bytes) {
  return MakePayload(bytes, PayloadKindFromEnv());
}

/** @brief User+system CPU seconds for the whole process, all threads. */
inline double ProcessCPUSeconds() {
#ifdef _WIN32
  FILETIME creation, exit_time, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel,
                       &user)) {
    return 0.0;
  }
  auto to_seconds = [](const FILETIME& f) {
    return static_cast<double>((static_cast<uint64_t>(f.dwHighDateTime) << 32) |
                               f.dwLowDateTime) *
           1e-7;
  };
  return to_seconds(kernel) + to_seconds(user);
#else
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0.0;
  }
  auto to_seconds = [](const timeval& tv) {
    return static_cast<double>(tv.tv_sec) +
           static_cast<double>(tv.tv_usec) * 1e-6;
  };
  return to_seconds(ru.ru_utime) + to_seconds(ru.ru_stime);
#endif
}

/** @brief ZNET_BENCH_REPS, clamped to [1, 99]. */
inline int Reps() {
  static int reps = []() {
    const char* s = std::getenv("ZNET_BENCH_REPS");
    int v = s ? std::atoi(s) : 1;
    if (v < 1) {
      v = 1;
    }
    if (v > 99) {
      v = 99;
    }
    return v;
  }();
  return reps;
}

// ---- CSV output --------------------------------------------------------------
// One wide table; fields not meaningful for a row's kind stay empty. Appended,
// so several binaries can share one file. For local aggregation across reps and
// runs; not stable enough across machines to gate anything on.

inline std::string& CsvImpairment() {
  static std::string desc = "none";
  return desc;
}

inline FILE* CsvFile() {
  static FILE* file = []() -> FILE* {
    const char* path = std::getenv("ZNET_BENCH_CSV");
    if (!path) {
      return nullptr;
    }
    FILE* f = std::fopen(path, "a");
    if (!f) {
      std::fprintf(stderr, "ZNET_BENCH_CSV: cannot open '%s'\n", path);
      return nullptr;
    }
    if (std::ftell(f) == 0) {
      std::fprintf(f,
                   "kind,library,transport,case,rep,payload,impairment,"
                   "delivered,seconds,msg_per_s,mib_per_s,timed_out,"
                   "cpu_us_per_msg,rtt_count,mean_us,p50_us,p95_us,p99_us,"
                   "probes_lost,ramp_ms,steady_msg_per_s,peak_msg_per_s,"
                   "separation\n");
    }
    return f;
  }();
  return file;
}

struct CsvRow {
  const char* kind = "";
  const char* library = "";
  const char* transport = "";
  std::string case_name;
  int rep = 1;
  double delivered = NAN;
  double seconds = NAN;
  double msg_per_s = NAN;
  double mib_per_s = NAN;
  int timed_out = -1;  // -1 = not applicable
  double cpu_us_per_msg = NAN;
  double rtt_count = NAN;
  double mean_us = NAN;
  double p50_us = NAN;
  double p95_us = NAN;
  double p99_us = NAN;
  double probes_lost = NAN;
  double ramp_ms = NAN;
  double steady_msg_per_s = NAN;
  double peak_msg_per_s = NAN;
  const char* separation = "";
};

inline void EmitCsv(const CsvRow& r) {
  FILE* f = CsvFile();
  if (!f) {
    return;
  }
  auto num = [f](double v) {
    if (!std::isnan(v)) {
      std::fprintf(f, "%.6g", v);
    }
    std::fputc(',', f);
  };
  std::fprintf(f, "%s,%s,%s,%s,%d,%s,\"%s\",", r.kind, r.library, r.transport,
               r.case_name.c_str(), r.rep, PayloadKindName(PayloadKindFromEnv()),
               CsvImpairment().c_str());
  num(r.delivered);
  num(r.seconds);
  num(r.msg_per_s);
  num(r.mib_per_s);
  if (r.timed_out >= 0) {
    std::fprintf(f, "%d", r.timed_out);
  }
  std::fputc(',', f);
  num(r.cpu_us_per_msg);
  num(r.rtt_count);
  num(r.mean_us);
  num(r.p50_us);
  num(r.p95_us);
  num(r.p99_us);
  num(r.probes_lost);
  num(r.ramp_ms);
  num(r.steady_msg_per_s);
  num(r.peak_msg_per_s);
  std::fprintf(f, "%s\n", r.separation);
  std::fflush(f);
}

// ---- measurement loops -------------------------------------------------------

/** @brief One throughput measurement. Timing and CPU cover the measured phase only. */
struct LoopResult {
  uint32_t delivered = 0;
  double seconds = 0.0;
  bool timed_out = false;   // hit the 60 s deadline short of the workload
  double cpu_seconds = 0.0;  // process-wide, all threads
};

inline double MsgsPerSecond(const LoopResult& r) {
  return r.seconds > 0 ? static_cast<double>(r.delivered) / r.seconds : 0.0;
}

// Drives one throughput case through the same pacing and backlog bound for
// every library. `send_one` puts a message on the wire or returns false;
// `pump` services the library and returns deliveries since the last call.
//
// `warmup` runs the same traffic untimed first and then quiesces, so a
// controller's ramp is not averaged into the measured rate. Impaired runs
// scale their message counts down far enough that slow-start would otherwise
// dominate the row.
template <typename SendOne, typename Pump>
LoopResult RunThroughputLoop(const Workload& w, SendOne send_one, Pump pump,
                             Clock::duration warmup = Clock::duration::zero()) {
  constexpr uint32_t kMaxBacklog = 4096;
  constexpr auto kDeadline = std::chrono::seconds(60);

  if (warmup > Clock::duration::zero()) {
    uint32_t sent = 0;
    uint32_t received = 0;
    auto warm_end = Clock::now() + warmup;
    while (Clock::now() < warm_end) {
      while (sent - received < kMaxBacklog) {
        if (!send_one()) {
          break;
        }
        sent++;
      }
      received += pump();
    }
    // drain, so warmup deliveries cannot leak into the measured count
    auto quiesce = Clock::now() + std::chrono::seconds(5);
    while (received < sent && Clock::now() < quiesce) {
      received += pump();
    }
  }

  LoopResult out;
  uint32_t sent = 0;
  uint32_t received = 0;
  const double cpu_start = ProcessCPUSeconds();
  auto start = Clock::now();
  auto deadline = start + kDeadline;
  while (received < w.messages && Clock::now() < deadline) {
    while (sent < w.messages && sent - received < kMaxBacklog) {
      if (!send_one()) {
        break;  // send queue full; drain and retry
      }
      sent++;
    }
    received += pump();
  }
  out.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  out.cpu_seconds = ProcessCPUSeconds() - cpu_start;
  out.delivered = received;
  out.timed_out = received < w.messages;
  return out;
}

// One ping-pong round trip per iteration, timed. Stops at the first timeout
// rather than recording a bogus sample.
template <typename SendOne, typename Pump>
std::vector<double> RunLatencyLoop(const Workload& w, SendOne send_one,
                                   Pump pump) {
  constexpr auto kTimeout = std::chrono::seconds(5);
  std::vector<double> samples;
  samples.reserve(w.messages);
  for (uint32_t i = 0; i < w.messages; i++) {
    auto sent_at = Clock::now();
    if (!send_one()) {
      break;
    }
    bool echoed = false;
    auto deadline = sent_at + kTimeout;
    while (!echoed && Clock::now() < deadline) {
      echoed = pump();
    }
    if (!echoed) {
      break;
    }
    samples.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - sent_at)
            .count());
  }
  return samples;
}

class Percentiles {
 public:
  explicit Percentiles(std::vector<double> samples) : samples_(std::move(samples)) {
    std::sort(samples_.begin(), samples_.end());
  }
  bool empty() const { return samples_.empty(); }
  size_t count() const { return samples_.size(); }
  double At(double q) const {
    if (samples_.empty()) {
      return 0.0;
    }
    double pos = q * static_cast<double>(samples_.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(pos));
    size_t hi = static_cast<size_t>(std::ceil(pos));
    double frac = pos - static_cast<double>(lo);
    return samples_[lo] * (1.0 - frac) + samples_[hi] * frac;
  }
  double Mean() const {
    if (samples_.empty()) {
      return 0.0;
    }
    double sum = 0.0;
    for (double s : samples_) {
      sum += s;
    }
    return sum / static_cast<double>(samples_.size());
  }

 private:
  std::vector<double> samples_;
};

// ---- reporting ---------------------------------------------------------------

inline void PrintHeader(const char* library, const char* transport) {
  std::printf("\n=== %s / %s ===\n", library, transport);
}

inline void Note(const char* text) {
  std::printf("  note: %s\n", text);
}

/** @brief Prints non-default run settings once, so a table is self-describing. */
inline void AnnounceRunSettings() {
  if (PayloadKindFromEnv() != PayloadKind::Binary) {
    std::printf("  payload: %s (ZNET_BENCH_PAYLOAD)\n",
                PayloadKindName(PayloadKindFromEnv()));
  }
  if (Reps() > 1) {
    std::printf("  reps: %d per case; rows are the median rep\n", Reps());
  }
  if (CsvFile() != nullptr) {
    std::printf("  csv: appending rows to $ZNET_BENCH_CSV\n");
  }
}

// One throughput row: the median rep by msg/s, with the rep span when N > 1
// and TIMEOUT when the median rep hit the deadline. CSV gets every rep.
inline void ReportThroughput(const char* library, const char* transport,
                             const Workload& w,
                             const std::vector<LoopResult>& reps) {
  if (reps.empty()) {
    return;
  }
  for (size_t i = 0; i < reps.size(); i++) {
    CsvRow row;
    row.kind = "throughput";
    row.library = library;
    row.transport = transport;
    row.case_name = w.name;
    row.rep = static_cast<int>(i + 1);
    row.delivered = reps[i].delivered;
    row.seconds = reps[i].seconds;
    row.msg_per_s = MsgsPerSecond(reps[i]);
    row.mib_per_s = reps[i].seconds > 0
                        ? (static_cast<double>(reps[i].delivered) *
                           static_cast<double>(w.payload_bytes)) /
                              reps[i].seconds / (1024.0 * 1024.0)
                        : 0.0;
    row.timed_out = reps[i].timed_out ? 1 : 0;
    row.cpu_us_per_msg = reps[i].delivered > 0
                             ? reps[i].cpu_seconds * 1e6 /
                                   static_cast<double>(reps[i].delivered)
                             : NAN;
    EmitCsv(row);
  }

  std::vector<LoopResult> sorted = reps;
  std::sort(sorted.begin(), sorted.end(),
            [](const LoopResult& a, const LoopResult& b) {
              return MsgsPerSecond(a) < MsgsPerSecond(b);
            });
  const LoopResult& mid = sorted[sorted.size() / 2];
  double msgs = MsgsPerSecond(mid);
  double mib = mid.seconds > 0 ? (static_cast<double>(mid.delivered) *
                                  static_cast<double>(w.payload_bytes)) /
                                     mid.seconds / (1024.0 * 1024.0)
                               : 0.0;
  double cpu_us = mid.delivered > 0
                      ? mid.cpu_seconds * 1e6 /
                            static_cast<double>(mid.delivered)
                      : 0.0;
  std::printf("%-10s %-6s throughput %-6s  %8u msgs  %8.3f s  %10.0f msg/s  %8.1f MiB/s  %7.2f cpu-us/msg",
              library, transport, w.name, mid.delivered, mid.seconds, msgs, mib,
              cpu_us);
  if (mid.timed_out) {
    std::printf("  TIMEOUT (%u/%u in 60 s)", mid.delivered, w.messages);
  }
  if (reps.size() > 1) {
    std::printf("  [%zu reps: %.0f..%.0f msg/s]", reps.size(),
                MsgsPerSecond(sorted.front()), MsgsPerSecond(sorted.back()));
  }
  std::printf("\n");
}

// One latency row from all reps' samples pooled. CSV gets per-rep percentiles.
inline void ReportLatency(const char* library, const char* transport,
                          const Workload& w,
                          const std::vector<std::vector<double>>& rep_samples) {
  std::vector<double> pooled;
  for (size_t i = 0; i < rep_samples.size(); i++) {
    Percentiles p(rep_samples[i]);
    CsvRow row;
    row.kind = "latency";
    row.library = library;
    row.transport = transport;
    row.case_name = w.name;
    row.rep = static_cast<int>(i + 1);
    row.rtt_count = static_cast<double>(p.count());
    row.mean_us = p.Mean();
    row.p50_us = p.At(0.50);
    row.p95_us = p.At(0.95);
    row.p99_us = p.At(0.99);
    EmitCsv(row);
    pooled.insert(pooled.end(), rep_samples[i].begin(), rep_samples[i].end());
  }
  Percentiles p(std::move(pooled));
  std::printf("%-10s %-6s latency    %-6s  %8zu rtt   mean %7.1f us  p50 %7.1f  p95 %7.1f  p99 %7.1f",
              library, transport, w.name, p.count(), p.Mean(), p.At(0.50),
              p.At(0.95), p.At(0.99));
  if (rep_samples.size() > 1) {
    std::printf("  (%zu reps pooled)", rep_samples.size());
  }
  std::printf("\n");
}

inline void ReportConnect(const char* library, const char* transport,
                          Clock::duration elapsed) {
  double ms = std::chrono::duration<double, std::milli>(elapsed).count();
  std::printf("%-10s %-6s connect                        %8.2f ms\n", library,
              transport, ms);
}

}  // namespace bench

#endif  // ZNET_BENCH_HARNESS_H
