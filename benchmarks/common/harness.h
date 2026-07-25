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

#ifndef ZNET_BENCH_HARNESS_H
#define ZNET_BENCH_HARNESS_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

// The same workload is run against every library so the numbers are comparable.
struct Workload {
  const char* name;
  size_t payload_bytes;
  uint32_t messages;
};

// Payload sizes chosen to exercise distinct paths: a small one that fits any
// datagram, one near a typical MTU, and one large enough to force fragmentation
// in a datagram transport.
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

// How compressible the payload is. There is no single realistic payload, and
// compression is worth wildly different amounts depending on the traffic, so
// the kinds are measured separately instead of one being picked as "typical".
// Filling a buffer with one repeated byte, the obvious default, is the worst
// choice of all: zstd takes 64 bytes of 'x' down to about 15, so several times
// as many messages fit a datagram and the result describes the payload rather
// than the transport.
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
      // An array of {id, position[3], velocity[3], flags}. Entities sit in a
      // bounded world moving at bounded speed, so the exponent bytes of the
      // floats repeat heavily while the mantissas do not, and ids and flags are
      // small. That mix is what makes real game state compress a little rather
      // than a lot.
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

// The comparison libraries do not compress, so the shared tables use the kind
// that compression cannot exploit. That keeps the throughput columns about the
// transport rather than about zstd.
inline std::string MakePayload(size_t bytes) {
  return MakePayload(bytes, PayloadKind::Binary);
}

// Drives one throughput case. `send_one` puts a single message on the wire and
// returns false if it could not; `pump` services the library and returns how
// many messages were delivered since the last call. Every library goes through
// this, so the send pacing and the backlog bound are identical across the
// comparison rather than reimplemented per benchmark.
//
// The backlog bound matters: handing a library every message at once measures
// how fast it can memcpy into its own send queue, not the protocol.
template <typename SendOne, typename Pump>
uint32_t RunThroughputLoop(const Workload& w, SendOne send_one, Pump pump) {
  constexpr uint32_t kMaxBacklog = 4096;
  constexpr auto kDeadline = std::chrono::seconds(60);
  uint32_t sent = 0;
  uint32_t received = 0;
  auto deadline = Clock::now() + kDeadline;
  while (received < w.messages && Clock::now() < deadline) {
    while (sent < w.messages && sent - received < kMaxBacklog) {
      if (!send_one()) {
        break;  // send queue is full; drain and retry
      }
      sent++;
    }
    received += pump();
  }
  return received;
}

// One ping-pong round trip per iteration, timed. `send_one` writes the probe,
// `pump` services both ends and returns true once the echo is back. Stops at
// the first timeout rather than recording a bogus sample.
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

inline void PrintHeader(const char* library, const char* transport) {
  std::printf("\n=== %s / %s ===\n", library, transport);
}

// One throughput row. `elapsed` covers only the measured phase, never setup.
inline void ReportThroughput(const char* library, const char* transport,
                             const Workload& w, uint32_t delivered,
                             Clock::duration elapsed) {
  double seconds = std::chrono::duration<double>(elapsed).count();
  double msgs = seconds > 0 ? delivered / seconds : 0.0;
  double mib = seconds > 0
                   ? (static_cast<double>(delivered) *
                      static_cast<double>(w.payload_bytes)) /
                         seconds / (1024.0 * 1024.0)
                   : 0.0;
  std::printf("%-10s %-6s throughput %-6s  %8u msgs  %8.3f s  %10.0f msg/s  %8.1f MiB/s\n",
              library, transport, w.name, delivered, seconds, msgs, mib);
}

// One latency row, in microseconds. Samples are per-message round trips.
inline void ReportLatency(const char* library, const char* transport,
                          const Workload& w, const Percentiles& p) {
  std::printf("%-10s %-6s latency    %-6s  %8zu rtt   mean %7.1f us  p50 %7.1f  p95 %7.1f  p99 %7.1f\n",
              library, transport, w.name, p.count(), p.Mean(), p.At(0.50),
              p.At(0.95), p.At(0.99));
}

inline void ReportConnect(const char* library, const char* transport,
                          Clock::duration elapsed) {
  double ms = std::chrono::duration<double, std::milli>(elapsed).count();
  std::printf("%-10s %-6s connect                        %8.2f ms\n", library,
              transport, ms);
}

inline void Note(const char* text) {
  std::printf("  note: %s\n", text);
}

}  // namespace bench

#endif  // ZNET_BENCH_HARNESS_H
