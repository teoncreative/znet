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
// Connection options, scoped like Netty's: `options` configure the thing you
// created (a listener, or a client), `child_options` configure each session that
// listener accepts. A client has no children, so its `options` are the session's.
//
//   ServerConfig config{"0.0.0.0", 25000, std::chrono::seconds(10)};
//   config.options.max_connections = 4096;   // listener
//   config.child_options.tcp.no_delay = true;
//   config.child_options.zdt.cwnd = 128;     // every accepted session
//
// Options are plain structs rather than a typed-key map: every option is known
// at compile time here, so a map would buy nothing and cost a lookup. Unset
// fields simply keep the defaults below.
//

#ifndef ZNET_PARENT_OPTIONS_H
#define ZNET_PARENT_OPTIONS_H

#include "znet/compression.h"
#include "znet/inet_addr.h"
#include "znet/precompiled.h"
#include "znet/types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace znet {

/** @brief Options that apply to any session, whatever its transport. */
struct CommonOptions {
  /**
   * @brief Drop a session that has heard nothing from its peer for this long.
   *
   * Both transports implement it, each against its own receive timestamps.
   * Zero disables it, leaving liveness to the OS and the peer.
   */
  std::chrono::milliseconds idle_timeout{10000};

  /**
   * @brief How often to ping a connection with nothing else to send.
   *
   * What keeps an honest but quiet peer inside the other end's idle_timeout,
   * and NAT bindings warm. The probe is transport-internal, a control frame
   * on TCP and a flag datagram on ZDT, and never reaches the application.
   * Zero disables it.
   */
  std::chrono::milliseconds keepalive_interval{1000};

  /** @brief Collect per-session counters. Ignored unless built with
   * ZNET_ENABLE_METRICS. */
  bool collect_metrics = true;

  /**
   * @brief Whether accepted sessions run the key exchange.
   *
   * Read only on the accepting side, i.e. a server's `child_options`. The
   * server announces its choice during the handshake and the client adopts it,
   * so setting this in ClientConfig::options has no effect. A client cannot
   * downgrade a server that requires encryption. For a P2P pair the dialer
   * marks exactly one peer as the initiator, so the other one decides.
   *
   * Turn it off only on an already-trusted transport, or to measure what the
   * crypto costs.
   */
  bool encryption = true;

  /**
   * @brief Compression applied to outgoing messages once the session is ready.
   *
   * Server-side like `encryption`, and negotiated the same way, so both ends
   * agree. Runs before encryption and therefore compresses the plaintext,
   * which works on encrypted and unencrypted sessions alike.
   */
  CompressionType compression = CompressionType::Default;

  /**
   * @brief Messages below this many bytes are sent uncompressed.
   *
   * A small payload cannot pay back the frame header, whatever it contains: at
   * 64 bytes zstd makes every kind of traffic about 12% *larger*, and it still
   * costs a full pass to build the coder tables. Measured break-even is near
   * 96 bytes for text and 128 for binary game state, so the default sits at the
   * point where compressing is no longer actively harmful.
   *
   * The type byte is per message, so a session freely mixes compressed and
   * uncompressed ones. Zero compresses everything.
   */
  size_t compression_threshold = 128;

  /**
   * @brief Packets a session will hold for its worker to encode.
   *
   * SendPacket() only queues, and returning false once the queue is full is
   * how an application learns it is outrunning the link. A transport queues
   * further messages behind its own congestion window, so this is not the
   * total a session can hold.
   *
   * The queue is a ring allocated whole when the session is constructed,
   * roughly 32 bytes a slot rounded up to a power of two, and nothing is
   * allocated per message afterwards. A refusal loses nothing, since the
   * caller still holds the packet, so this only has to cover the largest burst
   * worth absorbing between two of the worker's ticks.
   */
  size_t send_queue_capacity = 512;

  /**
   * @brief Log a hex dump of a decoded payload when a frame in it fails to
   *        decode.
   *
   * Once framing is untrustworthy the bytes are the only evidence, so this is
   * the knob to turn while diagnosing one. Capped at 512 bytes per buffer.
   * Off by default: payloads are user data, and this puts them in the log.
   */
  bool dump_on_decode_failure = false;

  /**
   * @brief Close the session once this many of its frames have failed to
   *        decode.
   *
   * Counted over the session's whole life, and a threshold rather than a
   * first-offence rule: one bad frame can be a bug, a stream of them is a
   * peer whose framing cannot be trusted. Every failure already costs the
   * rest of its buffer, so there is no reason to keep paying that
   * indefinitely. Unknown packet ids do not count toward this; see
   * DecodeStats. Zero disables it.
   */
  uint32_t max_invalid_frames = 16;
};

/** @brief TCP-specific socket options. */
struct TCPOptions {
  /** @brief Disable Nagle's algorithm (TCP_NODELAY). */
  bool no_delay = true;
  /** @brief Allow rebinding a port still in TIME_WAIT (SO_REUSEADDR). */
  bool reuse_address = true;
  /** @brief Send buffer size in bytes. Zero keeps the OS default. */
  int send_buffer_size = 0;
  /** @brief Receive buffer size in bytes. Zero keeps the OS default. */
  int receive_buffer_size = 0;
};

/**
 * @brief Candidate MTUs for the handshake to probe, largest first.
 *
 * Set them with Set(); the handshake steps down the list until one gets
 * through.
 */
struct MTULadder {
  static constexpr size_t kCapacity = 4;
  // fixed capacity and inline: a session copies its options, so a std::vector
  // would mean a heap allocation per connection to hold a handful of bytes
  std::array<uint16_t, kCapacity> rungs{1492, 1200, 576, 0};
  uint8_t count = 3;

  ZNET_CONSTEXPR17 void Set(std::initializer_list<uint16_t> values) {
    count = 0;
    for (uint16_t v : values) {
      if (count == kCapacity) {
        break;
      }
      rungs[count++] = v;
    }
  }
  constexpr const uint16_t* begin() const { return &rungs[0]; }
  constexpr const uint16_t* end() const { return &rungs[0] + count; }
  constexpr uint16_t front() const { return rungs[0]; }
  constexpr uint16_t back() const { return rungs[count ? count - 1u : 0u]; }
  constexpr bool empty() const { return count == 0; }
};

/** @brief ZDT tunables. */
struct ZDTOptions {
  /** @brief Candidate MTUs, probed largest first during the handshake. */
  MTULadder mtu_ladder;
  /** @brief Floor on the retransmit timeout, however low the measured RTT. */
  std::chrono::milliseconds rto_min{100};
  /** @brief Ceiling on the retransmit timeout, including backoff. */
  std::chrono::milliseconds rto_max{2000};
  /** @brief Retransmits of one message before the connection is closed. */
  int max_retries = 10;
  /**
   * @brief Ceiling on the congestion window, in datagrams.
   *
   * This is a bound, not the window itself: ZDT slow-starts from 10 datagrams
   * and backs off on queueing delay rather than on loss, and this caps how far
   * it may grow. Acknowledgements are run-length encoded, so the ceiling comes
   * from how far back the receiver remembers arrivals (kZDTAckHistoryBits)
   * rather than from header size.
   *
   * The window counts *datagrams*, not bytes, so a half-full datagram costs as
   * much of it as a full one. Bytes in flight are bounded by roughly this times
   * the MTU per round trip, which is what governs throughput on a long link.
   */
  int max_datagrams_in_flight = 512;
  /**
   * @brief Reliable messages allowed in flight. A memory bound, not congestion
   * control.
   *
   * Coalescing puts many messages in one datagram, so holding this near
   * max_datagrams_in_flight would throttle small messages well below what the
   * window actually allows.
   */
  int cwnd = 4096;
  /** @brief How long to wait for a handshake reply before resending. */
  std::chrono::milliseconds handshake_retransmit{250};
  /** @brief Handshake attempts at one MTU before stepping down the ladder. */
  int handshake_retries_per_rung = 4;
  /** @brief How often the server rotates its return-routability secret. */
  std::chrono::seconds cookie_secret_rotation{120};
  /** @brief Concurrent half-open handshakes a server will track. */
  int max_half_open = 1024;
  /** @brief Established connections a server will hold. */
  int max_connections = 4096;
  /** @brief Handshake messages accepted per source address per second. */
  int per_source_handshake_rate = 20;
  /** @brief Discard a partially reassembled message after this long. */
  std::chrono::milliseconds reassembly_timeout{5000};
  /**
   * @brief Ceiling on bytes held in partial reassemblies.
   *
   * Reaching it refuses to start new messages rather than discarding data
   * already accepted, so a peer stalls instead of losing anything.
   */
  size_t max_reassembly_bytes = 16u * 1024u * 1024u;

  /**
   * @brief SO_RCVBUF asked of the UDP socket. Zero leaves the OS default.
   *
   * One socket serves every connection, so this is per-endpoint, not
   * per-session. It only needs to absorb bursts between receive-thread
   * drains; max_inbox_datagrams is the designed backpressure point.
   *
   * Best-effort: the kernel clamps silently (net.core.rmem_max on Linux),
   * so the granted size is read back and logged at debug level.
   */
  int socket_recv_buffer = 4 * 1024 * 1024;
  /** @brief SO_SNDBUF, on the same terms as socket_recv_buffer. */
  int socket_send_buffer = 4 * 1024 * 1024;

  // the three below bound a flooding peer and an application that outruns the
  // link; each is a hard cap after which traffic is dropped or refused

  /** @brief Raw datagrams queued per connection before arrivals are dropped. */
  size_t max_inbox_datagrams = 4096;
  /**
   * @brief Encoded messages the transport will hold before Send() fails.
   *
   * Sizes the ring that whichever thread is encoding pushes into, allocated up
   * front on the same terms as CommonOptions::send_queue_capacity. The worker
   * also keeps a staging queue, where a shut congestion window parks messages,
   * and refills it from the ring only while it holds fewer than this many, so
   * the transport holds at most twice this figure in total.
   *
   * Unlike send_queue_capacity this sits past the point where a caller can be
   * told to try again: the message is already encoded, so a refusal here
   * drops it. Hence a default generous rather than tuned to the ~128KiB of
   * ring it implies. A server holding thousands of idle connections is the
   * case worth lowering it for.
   */
  size_t outbound_queue_capacity = 4096;
  /** @brief Concurrent partially reassembled messages. */
  size_t max_reassemblies = 256;
};

/**
 * @brief Per-session options. A server applies these to every session it
 * accepts, a client to its own.
 */
struct SessionOptions {
  CommonOptions common;
  TCPOptions tcp;
  ZDTOptions zdt;
};

/** @brief Listener-scope options: things that exist before any session does. */
struct ServerOptions {
  /** @brief Pending-connection backlog. Zero uses SOMAXCONN. TCP only. */
  int backlog = 0;
  /** @brief Cap on accepted connections. Zero means unlimited. */
  int max_connections = 0;
  /** @brief Allow rebinding a port still in TIME_WAIT (SO_REUSEADDR). */
  bool reuse_address = true;

  /**
   * @brief Sources allowed to connect. Empty admits everyone the denylist
   *        does not refuse; non-empty admits only what matches.
   *
   * Checked when a connection arrives: at accept on TCP, at first contact on
   * ZDT, where refusal is a silent drop so an excluded source learns nothing.
   * Unix sockets carry no address and bypass both lists; the socket file's
   * permissions are their gate. Invalid blocks are dropped with an error at
   * server construction, and sessions already accepted are never re-checked.
   */
  std::vector<CIDRBlock> allowlist;
  /** @brief Sources always refused. Wins over the allowlist. */
  std::vector<CIDRBlock> denylist;
  /**
   * @brief Connection attempts one source address may make per attempt_window.
   *
   * Zero disables it. Counted per attempt the server sees, which on ZDT
   * includes retransmits of a lost handshake datagram, so leave slack above
   * the honest rate. Tracking is per-IP with the port ignored, bounded in
   * memory, and fails open when full; see admission.cc.
   */
  uint32_t max_attempts_per_source = 0;
  /** @brief The window max_attempts_per_source is counted over. */
  std::chrono::milliseconds attempt_window{10000};
};

}  // namespace znet

#endif  // ZNET_PARENT_OPTIONS_H
