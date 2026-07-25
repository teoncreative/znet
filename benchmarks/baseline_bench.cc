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
// Raw blocking sockets with a length prefix and nothing else: no reliability,
// no ordering, no encryption, no allocation per message. This is the floor.
// Every library in these benchmarks pays for features this does not have, so
// read it as "what the syscalls alone cost on this machine", not as a rival.
//

#include "common/harness.h"

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

// --- TCP -----------------------------------------------------------------

void TCPThroughput(const bench::Workload& w) {
  uint16_t port = 0;
  int listener = MakeLoopback(SOCK_STREAM, &port);
  listen(listener, 1);

  std::atomic_uint32_t received{0};
  std::thread server([&]() {
    int conn = accept(listener, nullptr, nullptr);
    int one = 1;
    setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::vector<char> buf(w.payload_bytes);
    for (uint32_t i = 0; i < w.messages; i++) {
      uint32_t len = 0;
      if (!ReadExactly(conn, reinterpret_cast<char*>(&len), sizeof(len))) {
        break;
      }
      if (!ReadExactly(conn, buf.data(), ntohl(len))) {
        break;
      }
      received.fetch_add(1);
    }
    close(conn);
  });

  int client = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(port);
  connect(client, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  int one = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  const std::string payload = bench::MakePayload(w.payload_bytes);
  uint32_t net_len = htonl(static_cast<uint32_t>(w.payload_bytes));
  auto start = bench::Clock::now();
  for (uint32_t i = 0; i < w.messages; i++) {
    if (!WriteExactly(client, reinterpret_cast<const char*>(&net_len),
                      sizeof(net_len)) ||
        !WriteExactly(client, payload.data(), payload.size())) {
      break;
    }
  }
  server.join();
  auto elapsed = bench::Clock::now() - start;
  bench::ReportThroughput("baseline", "TCP", w, received.load(), elapsed);
  close(client);
  close(listener);
}

void TCPLatency(const bench::Workload& w) {
  uint16_t port = 0;
  int listener = MakeLoopback(SOCK_STREAM, &port);
  listen(listener, 1);

  std::thread server([&]() {
    int conn = accept(listener, nullptr, nullptr);
    int one = 1;
    setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::vector<char> buf(w.payload_bytes);
    for (uint32_t i = 0; i < w.messages; i++) {
      uint32_t len = 0;
      if (!ReadExactly(conn, reinterpret_cast<char*>(&len), sizeof(len))) {
        break;
      }
      if (!ReadExactly(conn, buf.data(), ntohl(len))) {
        break;
      }
      if (!WriteExactly(conn, reinterpret_cast<const char*>(&len), sizeof(len)) ||
          !WriteExactly(conn, buf.data(), ntohl(len))) {
        break;
      }
    }
    close(conn);
  });

  int client = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(port);
  connect(client, reinterpret_cast<sockaddr*>(&to), sizeof(to));
  int one = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

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
    if (!ReadExactly(client, reinterpret_cast<char*>(&len), sizeof(len)) ||
        !ReadExactly(client, buf.data(), ntohl(len))) {
      break;
    }
    samples.push_back(
        std::chrono::duration<double, std::micro>(bench::Clock::now() - sent)
            .count());
  }
  server.join();
  bench::ReportLatency("baseline", "TCP", w, bench::Percentiles(samples));
  close(client);
  close(listener);
}

// --- UDP -----------------------------------------------------------------
// Unreliable and unordered, so loss shows up as a delivered count below the
// message count. On loopback it is usually lossless until the socket buffer
// overflows, which is itself informative.

void UDPThroughput(const bench::Workload& w) {
  if (w.payload_bytes > 60000) {
    return;
  }
  uint16_t port = 0;
  int server_fd = MakeLoopback(SOCK_DGRAM, &port);
  int big = 16 * 1024 * 1024;
  setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));

  std::atomic_uint32_t received{0};
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
  auto start = bench::Clock::now();
  for (uint32_t i = 0; i < w.messages; i++) {
    sendto(client, payload.data(), payload.size(), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
  }
  auto deadline = bench::Clock::now() + std::chrono::seconds(3);
  while (received.load() < w.messages && bench::Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  auto elapsed = bench::Clock::now() - start;
  stop = true;
  server.join();

  bench::ReportThroughput("baseline", "UDP", w, received.load(), elapsed);
  if (received.load() < w.messages) {
    std::printf("             (%u of %u arrived; raw UDP has no retransmit)\n",
                received.load(), w.messages);
  }
  close(client);
  close(server_fd);
}

void UDPLatency(const bench::Workload& w) {
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
  bench::ReportLatency("baseline", "UDP", w, bench::Percentiles(samples));
  close(client);
  close(server_fd);
}

}  // namespace

int main() {
  std::printf("raw sockets (no reliability, ordering or encryption)\n");
  bench::PrintHeader("baseline", "TCP");
  for (const auto& w : bench::DefaultThroughputWorkloads()) {
    TCPThroughput(w);
  }
  TCPLatency(bench::DefaultLatencyWorkload());

  bench::PrintHeader("baseline", "UDP");
  for (const auto& w : bench::DefaultThroughputWorkloads()) {
    UDPThroughput(w);
  }
  UDPLatency(bench::DefaultLatencyWorkload());
  return 0;
}
