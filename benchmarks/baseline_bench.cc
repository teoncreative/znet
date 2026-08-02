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
// Raw blocking sockets with a length prefix and nothing else. This is the
// floor: read it as "what the syscalls alone cost", not as a rival. The TCP
// congestion row doubles as a kernel-CC (cubic) reference for the controller
// comparison.
//

#include "common/congestion.h"
#include "common/harness.h"
#include "common/impairment.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bench::Impairment g_impair;

int MakeLoopback(int type, uint16_t* out_port) {
  int fd = socket(AF_INET, type, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  *out_port = ntohs(addr.sin_port);
  return fd;
}

bool ReadExactly(int fd, char* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, buf + got, n - got, 0);
    if (r <= 0) {
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

bool WriteExactly(int fd, const char* buf, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = send(fd, buf + sent, n - sent, 0);
    if (w <= 0) {
      return false;
    }
    sent += static_cast<size_t>(w);
  }
  return true;
}

int ConnectLoopback(int type, uint16_t port) {
  int fd = socket(AF_INET, type, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(port);
  if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void SetNoDelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// --- TCP -----------------------------------------------------------------

void TCPThroughput(const bench::Workload& w) {
  std::vector<bench::LoopResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    uint16_t port = 0;
    int listener = MakeLoopback(SOCK_STREAM, &port);
    listen(listener, 1);

    std::atomic_uint32_t received{0};
    std::thread server([&]() {
      int conn = accept(listener, nullptr, nullptr);
      if (conn < 0) {
        return;
      }
      SetNoDelay(conn);
      std::vector<char> buf(w.payload_bytes);
      for (uint32_t i = 0; i < w.messages; i++) {
        uint32_t len = 0;
        if (!ReadExactly(conn, reinterpret_cast<char*>(&len), sizeof(len))) {
          break;
        }
        uint32_t n = ntohl(len);
        if (n > buf.size() || !ReadExactly(conn, buf.data(), n)) {
          break;
        }
        received.fetch_add(1);
      }
      close(conn);
    });

    int client = ConnectLoopback(SOCK_STREAM, port);
    if (client < 0) {
      std::printf("%-10s %-6s throughput %-6s  FAILED to connect\n", "baseline",
                  "TCP", w.name);
      shutdown(listener, SHUT_RDWR);  // wakes the blocked accept
      server.join();
      close(listener);
      continue;
    }
    SetNoDelay(client);

    const std::string payload = bench::MakePayload(w.payload_bytes);
    uint32_t net_len = htonl(static_cast<uint32_t>(w.payload_bytes));
    const double cpu_start = bench::ProcessCPUSeconds();
    auto start = bench::Clock::now();
    for (uint32_t i = 0; i < w.messages; i++) {
      if (!WriteExactly(client, reinterpret_cast<const char*>(&net_len),
                        sizeof(net_len)) ||
          !WriteExactly(client, payload.data(), payload.size())) {
        break;
      }
    }
    close(client);  // EOF unblocks the server if the client broke early
    server.join();
    bench::LoopResult r;
    r.delivered = received.load();
    r.seconds =
        std::chrono::duration<double>(bench::Clock::now() - start).count();
    r.cpu_seconds = bench::ProcessCPUSeconds() - cpu_start;
    r.timed_out = false;
    reps.push_back(r);
    close(listener);
  }
  bench::ReportThroughput("baseline", "TCP", w, reps);
}

void TCPLatency(const bench::Workload& w) {
  std::vector<std::vector<double>> rep_samples;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    uint16_t port = 0;
    int listener = MakeLoopback(SOCK_STREAM, &port);
    listen(listener, 1);

    std::thread server([&]() {
      int conn = accept(listener, nullptr, nullptr);
      if (conn < 0) {
        return;
      }
      SetNoDelay(conn);
      std::vector<char> buf(w.payload_bytes);
      for (uint32_t i = 0; i < w.messages; i++) {
        uint32_t len = 0;
        if (!ReadExactly(conn, reinterpret_cast<char*>(&len), sizeof(len))) {
          break;
        }
        uint32_t n = ntohl(len);
        if (n > buf.size() || !ReadExactly(conn, buf.data(), n)) {
          break;
        }
        if (!WriteExactly(conn, reinterpret_cast<const char*>(&len), sizeof(len)) ||
            !WriteExactly(conn, buf.data(), n)) {
          break;
        }
      }
      close(conn);
    });

    int client = ConnectLoopback(SOCK_STREAM, port);
    if (client < 0) {
      std::printf("%-10s %-6s latency    %-6s  FAILED to connect\n", "baseline",
                  "TCP", w.name);
      shutdown(listener, SHUT_RDWR);
      server.join();
      close(listener);
      continue;
    }
    SetNoDelay(client);

    const std::string payload = bench::MakePayload(w.payload_bytes);
    uint32_t net_len = htonl(static_cast<uint32_t>(w.payload_bytes));
    std::vector<char> buf(w.payload_bytes);
    std::vector<double> samples;
    samples.reserve(w.messages);
    for (uint32_t i = 0; i < w.messages; i++) {
      auto sent = bench::Clock::now();
      if (!WriteExactly(client, reinterpret_cast<const char*>(&net_len),
                        sizeof(net_len)) ||
          !WriteExactly(client, payload.data(), payload.size())) {
        break;
      }
      uint32_t len = 0;
      if (!ReadExactly(client, reinterpret_cast<char*>(&len), sizeof(len))) {
        break;
      }
      uint32_t n = ntohl(len);
      if (n > buf.size() || !ReadExactly(client, buf.data(), n)) {
        break;
      }
      samples.push_back(
          std::chrono::duration<double, std::micro>(bench::Clock::now() - sent)
              .count());
    }
    close(client);
    server.join();
    close(listener);
    rep_samples.push_back(std::move(samples));
  }
  bench::ReportLatency("baseline", "TCP", w, rep_samples);
}

// Kernel-CC reference for the congestion pool: cubic doing the bulk transfer,
// the probe on its own connection (sep=conn). Both share the netem qdisc, so
// the probe reads the queue the transfer built in the link, not in the socket.
void TCPCongestion(const bench::CongestionCase& c) {
  std::vector<bench::CongestionResult> reps;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    uint16_t port = 0;
    int listener = MakeLoopback(SOCK_STREAM, &port);
    listen(listener, 2);

    std::atomic_uint32_t delivered{0};
    std::thread server([&]() {
      int bulk_conn = accept(listener, nullptr, nullptr);  // client connects bulk first
      if (bulk_conn < 0) {
        return;
      }
      int probe_conn = accept(listener, nullptr, nullptr);
      if (probe_conn < 0) {
        close(bulk_conn);
        return;
      }
      SetNoDelay(bulk_conn);
      SetNoDelay(probe_conn);
      std::thread sink([&, bulk_conn]() {
        std::vector<char> buf(c.bulk_bytes);
        for (;;) {
          uint32_t len = 0;
          if (!ReadExactly(bulk_conn, reinterpret_cast<char*>(&len), sizeof(len))) {
            break;
          }
          uint32_t n = ntohl(len);
          if (n > buf.size() || !ReadExactly(bulk_conn, buf.data(), n)) {
            break;
          }
          delivered.fetch_add(1, std::memory_order_relaxed);
        }
      });
      std::vector<char> buf(c.probe_bytes);
      for (;;) {
        uint32_t len = 0;
        if (!ReadExactly(probe_conn, reinterpret_cast<char*>(&len), sizeof(len))) {
          break;
        }
        uint32_t n = ntohl(len);
        if (n > buf.size() || !ReadExactly(probe_conn, buf.data(), n)) {
          break;
        }
        if (!WriteExactly(probe_conn, reinterpret_cast<const char*>(&len),
                          sizeof(len)) ||
            !WriteExactly(probe_conn, buf.data(), n)) {
          break;
        }
      }
      sink.join();
      close(bulk_conn);
      close(probe_conn);
    });

    int bulk = ConnectLoopback(SOCK_STREAM, port);
    int probe = bulk >= 0 ? ConnectLoopback(SOCK_STREAM, port) : -1;
    if (bulk < 0 || probe < 0) {
      std::printf("%-10s %-6s congestion %-6s  FAILED to connect\n", "baseline",
                  "TCP", c.name);
      if (bulk >= 0) {
        close(bulk);
      }
      shutdown(listener, SHUT_RDWR);
      server.join();
      close(listener);
      continue;
    }
    SetNoDelay(bulk);
    SetNoDelay(probe);
    timeval tv{};
    tv.tv_sec = 2;  // probe timeout
    setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::atomic_bool stop{false};
    const std::string bulk_payload = bench::MakePayload(c.bulk_bytes);
    std::thread sender([&]() {
      uint32_t net_len = htonl(static_cast<uint32_t>(c.bulk_bytes));
      while (!stop.load(std::memory_order_relaxed)) {
        if (!WriteExactly(bulk, reinterpret_cast<const char*>(&net_len),
                          sizeof(net_len)) ||
            !WriteExactly(bulk, bulk_payload.data(), bulk_payload.size())) {
          break;
        }
      }
    });

    const std::string probe_payload = bench::MakePayload(c.probe_bytes);
    std::vector<char> echo(c.probe_bytes);
    bench::CongestionResult r;
    const double cpu_start = bench::ProcessCPUSeconds();
    const auto start = bench::Clock::now();
    const auto finish = start + c.duration;
    auto next_bucket = start + c.bucket;
    uint32_t bucket_base = 0;
    while (bench::Clock::now() < finish) {
      // drop a stale echo left by a timed-out probe
      char scratch[512];
      while (recv(probe, scratch, sizeof(scratch), MSG_DONTWAIT) > 0) {
      }
      uint32_t net_len = htonl(static_cast<uint32_t>(c.probe_bytes));
      auto sent_at = bench::Clock::now();
      if (!WriteExactly(probe, reinterpret_cast<const char*>(&net_len),
                        sizeof(net_len)) ||
          !WriteExactly(probe, probe_payload.data(), probe_payload.size())) {
        break;
      }
      uint32_t len = 0;
      bool got = ReadExactly(probe, reinterpret_cast<char*>(&len), sizeof(len));
      if (got) {
        uint32_t n = ntohl(len);
        got = n <= echo.size() && ReadExactly(probe, echo.data(), n);
      }
      if (got) {
        r.probe_us.push_back(std::chrono::duration<double, std::micro>(
                                 bench::Clock::now() - sent_at)
                                 .count());
      } else {
        r.probes_lost++;
      }
      while (bench::Clock::now() >= next_bucket) {
        uint32_t now_delivered = delivered.load(std::memory_order_relaxed);
        r.buckets.push_back(now_delivered - bucket_base);
        bucket_base = now_delivered;
        next_bucket += c.bucket;
      }
      std::this_thread::sleep_for(c.probe_every);
    }
    stop.store(true, std::memory_order_relaxed);
    shutdown(bulk, SHUT_RDWR);  // wakes a blocked send
    sender.join();
    close(bulk);
    close(probe);
    server.join();
    close(listener);

    r.bulk_delivered = delivered.load();
    r.elapsed = bench::Clock::now() - start;
    r.cpu_seconds = bench::ProcessCPUSeconds() - cpu_start;
    reps.push_back(std::move(r));
  }
  bench::ReportCongestionCase("baseline", "TCP", c, reps, "conn");
}

// --- UDP -----------------------------------------------------------------
// Unreliable and unordered: loss shows up as a short delivered count.

void UDPThroughput(const bench::Workload& w) {
  if (w.payload_bytes > 60000) {
    std::printf("%-10s %-6s throughput %-6s  skipped (exceeds a single datagram)\n",
                "baseline", "UDP", w.name);
    return;
  }
  std::vector<bench::LoopResult> reps;
  uint32_t min_delivered = w.messages;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    uint16_t port = 0;
    int server_fd = MakeLoopback(SOCK_DGRAM, &port);
    int big = 16 * 1024 * 1024;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));

    std::atomic_uint32_t received{0};
    // rep of the last arrival's time_since_epoch, so the drain wait below
    // never inflates the measured time
    std::atomic<bench::Clock::rep> last_arrival{0};
    std::atomic_bool stop{false};
    std::thread server([&]() {
      std::vector<char> buf(w.payload_bytes + 64);
      timeval tv{};
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      while (!stop.load() && received.load() < w.messages) {
        ssize_t r = recv(server_fd, buf.data(), buf.size(), 0);
        if (r > 0) {
          received.fetch_add(1);
          last_arrival.store(
              bench::Clock::now().time_since_epoch().count(),
              std::memory_order_relaxed);
        }
      }
    });

    int client = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(client, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(port);

    const std::string payload = bench::MakePayload(w.payload_bytes);
    const double cpu_start = bench::ProcessCPUSeconds();
    auto start = bench::Clock::now();
    for (uint32_t i = 0; i < w.messages; i++) {
      sendto(client, payload.data(), payload.size(), 0,
             reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    auto deadline = bench::Clock::now() + std::chrono::seconds(3);
    while (received.load() < w.messages && bench::Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double cpu_end = bench::ProcessCPUSeconds();
    stop = true;
    server.join();

    bench::LoopResult r;
    r.delivered = received.load();
    bench::Clock::rep last = last_arrival.load();
    auto end = last != 0
                   ? bench::Clock::time_point(bench::Clock::duration(last))
                   : bench::Clock::now();
    r.seconds = std::chrono::duration<double>(end - start).count();
    r.cpu_seconds = cpu_end - cpu_start;
    r.timed_out = false;  // loss is the result, not a failure
    min_delivered = std::min(min_delivered, r.delivered);
    reps.push_back(r);
    close(client);
    close(server_fd);
  }
  bench::ReportThroughput("baseline", "UDP", w, reps);
  if (min_delivered < w.messages) {
    std::printf("             (worst rep: %u of %u arrived; raw UDP has no retransmit)\n",
                min_delivered, w.messages);
  }
}

void UDPLatency(const bench::Workload& w) {
  std::vector<std::vector<double>> rep_samples;
  for (int rep = 0; rep < bench::Reps(); rep++) {
    uint16_t port = 0;
    int server_fd = MakeLoopback(SOCK_DGRAM, &port);
    std::atomic_bool stop{false};
    std::thread server([&]() {
      std::vector<char> buf(w.payload_bytes + 64);
      timeval tv{};
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      while (!stop.load()) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        ssize_t r = recvfrom(server_fd, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &flen);
        if (r > 0) {
          sendto(server_fd, buf.data(), static_cast<size_t>(r), 0,
                 reinterpret_cast<sockaddr*>(&from), flen);
        }
      }
    });

    int client = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(port);
    timeval tv{};
    tv.tv_sec = 1;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const std::string payload = bench::MakePayload(w.payload_bytes);
    std::vector<char> buf(w.payload_bytes + 64);
    std::vector<double> samples;
    samples.reserve(w.messages);
    for (uint32_t i = 0; i < w.messages; i++) {
      auto sent = bench::Clock::now();
      sendto(client, payload.data(), payload.size(), 0,
             reinterpret_cast<sockaddr*>(&to), sizeof(to));
      ssize_t r = recv(client, buf.data(), buf.size(), 0);
      if (r <= 0) {
        continue;  // dropped; raw UDP will not resend it
      }
      samples.push_back(
          std::chrono::duration<double, std::micro>(bench::Clock::now() - sent)
              .count());
    }
    stop = true;
    server.join();
    close(client);
    close(server_fd);
    rep_samples.push_back(std::move(samples));
  }
  bench::ReportLatency("baseline", "UDP", w, rep_samples);
}

}  // namespace

int main() {
  std::printf("raw sockets (no reliability, ordering or encryption)\n");
  g_impair = bench::Impairment::FromEnv();
  bench::NoteImpairment(g_impair);
  bench::AnnounceRunSettings();
  const bool skip_congestion =
      std::getenv("ZNET_BENCH_SKIP_CONGESTION") != nullptr;

  bench::PrintHeader("baseline", "TCP");
  for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
    TCPThroughput(w);
  }
  TCPLatency(bench::ImpairedLatencyWorkload(g_impair));
  if (!skip_congestion) {
    for (const auto& c : bench::DefaultCongestionCases()) {
      TCPCongestion(c);
    }
  }

  bench::PrintHeader("baseline", "UDP");
  for (const auto& w : bench::ImpairedThroughputWorkloads(g_impair)) {
    UDPThroughput(w);
  }
  UDPLatency(bench::ImpairedLatencyWorkload(g_impair));
  return 0;
}
