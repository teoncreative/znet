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
// ZDT (znet Datagram Transport). A self-contained reliable-UDP transport with
// channels (reliable/unreliable x ordered/unordered), a RakNet-style versioned
// and spoof-resistant handshake, fragmentation and keepalive. It slots in below
// the existing send/recv pipeline as a TransportLayer, so encryption, compression
// and serialization are unchanged. See docs/zdt-design.md for the full spec.
//

#ifndef ZNET_PARENT_ZDT_H
#define ZNET_PARENT_ZDT_H

#include "znet/backends/backend.h"
#include "znet/buffer.h"
#include "znet/inet_addr.h"
#include "znet/metrics.h"
#include "znet/options.h"
#include "znet/peer_session.h"
#include "znet/precompiled.h"
#include "znet/transport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace znet {
namespace backends {

// Protocol version, checked for strict equality during the handshake.
inline constexpr uint8_t kZDTProtocolVersion = 1;

// Prefix on offline (pre-connection) messages, so unrelated UDP traffic on the
// port is not parsed as a handshake.
inline constexpr std::array<uint8_t, 8> kZDTMagic = {'Z', 'N', 'E', 'T',
                                                     'Z', 'D', 'T', 0x01};

// Online datagram flags (byte 0). Bit 7 separates connected-state datagrams from
// offline handshake messages, which the demux keys on.
inline constexpr uint8_t kFlagData = 1u << 0;      // carries a message fragment
inline constexpr uint8_t kFlagReliable = 1u << 1;  // retransmit until acked
inline constexpr uint8_t kFlagOrdered = 1u << 2;   // ordering applies on channel
inline constexpr uint8_t kFlagFragment = 1u << 3;  // frag_index/frag_count present
inline constexpr uint8_t kFlagFin = 1u << 4;       // graceful close
inline constexpr uint8_t kFlagPing = 1u << 5;      // keepalive probe
inline constexpr uint8_t kFlagPong = 1u << 6;      // keepalive reply
inline constexpr uint8_t kFlagOnline = 1u << 7;    // online-datagram marker

// Offline (handshake) message ids, all < 0x80 so they never set kFlagOnline.
enum class ZDTOfflineMsg : uint8_t {
  OpenConnectionRequest1 = 0x01,
  OpenConnectionReply1 = 0x02,
  OpenConnectionRequest2 = 0x03,
  OpenConnectionReply2 = 0x04,
  IncompatibleProtocolVersion = 0x05,
  NoFreeConnections = 0x06,
  AlreadyConnected = 0x07,
  ConnectionBanned = 0x08,
  Punch = 0x09,  // P2P NAT hole-punch keepalive (not part of the client/server flow)
};

// Online datagram header. Base is 12 bytes; +2 when kFlagFragment is set.
inline constexpr size_t kZDTHeaderSize = 12;
inline constexpr size_t kZDTFragHeaderSize = 14;

// Sequences go on the wire truncated to 16 bits but are tracked in full: a
// truncated value aliases every 65536 messages, which would let a late retransmit
// reconstruct onto a live message and corrupt ordering and map keys.
using WireSeq = uint16_t;
using SequenceId = uint64_t;

// Rebuilds the full SequenceId from a truncated wire value, picking the
// candidate nearest `expected` (the standard TCP/QUIC reconstruction).
inline SequenceId ReconstructSeq(WireSeq truncated, SequenceId expected) {
  constexpr SequenceId kPeriod = SequenceId{1} << 16;
  SequenceId candidate = (expected & ~(kPeriod - 1)) | truncated;
  if (candidate + kPeriod / 2 < expected) {
    candidate += kPeriod;
  } else if (candidate >= expected + kPeriod / 2 && candidate >= kPeriod) {
    candidate -= kPeriod;
  }
  return candidate;
}

struct ZDTHeader {
  uint8_t flags = kFlagOnline;
  uint8_t channel = 0;
  uint16_t packet_seq = 0;   // connection-level, ++ per datagram (drives ack/RTT)
  uint16_t ack = 0;          // highest packet_seq seen from peer
  uint32_t ack_bits = 0;     // bitfield of the 32 packet_seqs before `ack`
  uint16_t message_seq = 0;  // per-channel message sequence (DATA)
  uint8_t frag_index = 0;
  uint8_t frag_count = 1;
};

// Serializes `header` (big-endian) to the front of `buffer`.
void WriteZDTHeader(Buffer& buffer, const ZDTHeader& header);
// Parses an online header from the front of `buffer`; returns false if the buffer
// is too short or does not carry the online marker. Leaves the read cursor at the
// start of the payload on success.
bool ReadZDTHeader(Buffer& buffer, ZDTHeader& out_header);

// Writes an offline message id followed by kZDTMagic.
void WriteOfflineHeader(Buffer& buffer, ZDTOfflineMsg id);
// Reads and validates an offline header (id < 0x80 and correct magic). On success
// the read cursor is left just past the magic and `out_id` holds the message id.
bool ReadOfflineHeader(Buffer& buffer, ZDTOfflineMsg& out_id);

// --- Return-routability cookie ------------------------------------------------
// Server issues HMAC(secret[epoch], addr, epoch) in Reply1 holding no state, and
// only allocates a session once the client echoes it back in Request2.
inline constexpr size_t kZDTCookieLen = 16;
using ZDTCookie = std::array<uint8_t, kZDTCookieLen>;

ZDTCookie ComputeCookie(const uint8_t* secret, size_t secret_len,
                        const std::string& peer_readable, uint32_t epoch);
// Constant-time comparison (no early-out) to avoid timing side channels.
bool ConstTimeEqual(const ZDTCookie& a, const ZDTCookie& b);
// 64-bit random peer identifier (OpenSSL RAND_bytes).
uint64_t GenerateGuid();

enum class RecvResult { Received, WouldBlock, Error };

// Thin owner of a UDP socket. Shared by a server's per-peer transports: concurrent
// sendto() on one socket is safe.
class UDPSocket {
 public:
  UDPSocket() = default;
  ~UDPSocket();
  UDPSocket(const UDPSocket&) = delete;
  UDPSocket& operator=(const UDPSocket&) = delete;

  Result Open(InetProtocolVersion ipv);
  Result Bind(const InetAddress& addr);

  bool SendTo(const InetAddress& addr, const void* data, size_t len);
  RecvResult RecvFrom(void* data, size_t cap, size_t& out_len,
                      std::shared_ptr<InetAddress>& out_from);

  bool SetBlocking(bool blocking);
  bool SetReceiveTimeout(std::chrono::milliseconds timeout);
  // Best-effort; the handshake MTU probe needs oversized datagrams dropped
  // rather than IP-fragmented.
  bool SetDontFragment(bool enabled);

  Result Close();
  bool IsValid() const { return IsValidSocketHandle(socket_); }
  SocketHandle handle() const { return socket_; }
  std::shared_ptr<InetAddress> local_address();

 private:
  SocketHandle socket_ = kSocketInvalid;
};

// Thread-safe raw-datagram queue, shared by a producer and a consumer running on
// different threads (see ZDTTransportLayer for the threading rule).
class ZDTInbox {
 public:
  // Drops and returns false once `limit` datagrams are pending, so a flooding
  // peer cannot grow this queue without bound.
  bool Push(const uint8_t* data, size_t len, size_t limit);
  void Drain(std::deque<std::vector<uint8_t>>& out);
  size_t dropped() const;

 private:
  mutable std::mutex mutex_;
  std::deque<std::vector<uint8_t>> queue_;
  size_t dropped_ = 0;
};

// Per-connection parameters settled by the handshake.
struct ZDTConnection {
  uint16_t mtu = 1200;
  uint64_t local_guid = 0;
  uint64_t remote_guid = 0;
};

// Per-peer transport. All protocol state is touched only on the owning session's
// worker thread (Update/Receive/Send). OnDatagram may be called from any thread;
// it just appends raw bytes to the inbox which Update() drains.
class ZDTTransportLayer : public TransportLayer {
 public:
  ZDTTransportLayer(std::shared_ptr<UDPSocket> socket,
                    std::shared_ptr<InetAddress> peer, ZDTOptions config,
                    bool drains_own_socket, std::shared_ptr<ZDTInbox> inbox,
                    ZDTConnection connection);
  ~ZDTTransportLayer() override;

  std::shared_ptr<Buffer> Receive() override;
  bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) override;
  Result Close(CloseOptions options = {}) override;
  bool IsClosed() override { return is_closed_; }
  void Update() override;

  // Feeds one raw ZDT datagram (UDP payload) to this transport. Thread-safe.
  void OnDatagram(const uint8_t* data, size_t len);

  void FillMetrics(SessionMetrics& out) const override;

  std::shared_ptr<InetAddress> peer() const { return peer_; }
  std::shared_ptr<ZDTInbox> inbox() const { return inbox_; }

 private:
  using TimePoint = std::chrono::steady_clock::time_point;

  // Identifies one reliable datagram / reassembly buffer.
  struct MsgKey {
    uint8_t channel = 0;
    bool reliable = false;
    SequenceId message_seq = 0;
    uint8_t frag_index = 0;
    bool operator<(const MsgKey& other) const {
      return std::tie(channel, reliable, message_seq, frag_index) <
             std::tie(other.channel, other.reliable, other.message_seq,
                      other.frag_index);
    }
  };

  void DrainSocket();     // client-side: recvfrom own socket -> inbox
  void ProcessInbound();  // parse queued raw datagrams (worker thread)
  void FlushOutbound();   // send queued NEW messages (worker thread)
  void SendControl(uint8_t flags);
  void CheckTimers();

  // low-level send: assigns a fresh packet_seq, piggybacks the current ack, and
  // records the packet for RTT/reliability. `extra_flags` excludes kFlagOnline;
  // frag fields are only serialized when kFlagFragment is set in extra_flags.
  WireSeq SendDatagram(uint8_t extra_flags, uint8_t channel,
                       SequenceId message_seq, uint8_t frag_index,
                       uint8_t frag_count, const char* payload,
                       size_t payload_len, bool reliable,
                       const MsgKey& reliable_key);
  void FillAck(ZDTHeader& header) const;
  void RecordRemoteSeq(WireSeq packet_seq);   // update our (ack, ack_bits)
  void ProcessAcks(const ZDTHeader& header);  // consume peer's (ack, ack_bits)
  void AckPacket(WireSeq packet_seq);
  void UpdateRtt(std::chrono::steady_clock::duration sample);
  void RetransmitUnacked();
  void PruneSentPackets();
  // Expands the header's truncated message_seq to a full SequenceId, using the
  // matching substream's position on that channel as context.
  SequenceId ReconstructSeqFor(const ZDTHeader& header);
  void OnDataFragment(const ZDTHeader& header, const uint8_t* data, size_t len);
  void PruneReassembly();
  void DeliverMessage(const ZDTHeader& header, std::shared_ptr<Buffer> payload);

  // Ring of the most recent packet_seqs a reliable datagram was sent under.
  // Fixed capacity, stored inline, so tracking retransmissions never allocates
  // and never grows regardless of how many retries a configuration allows.
  struct TransmissionLog {
    static constexpr size_t kCapacity = 16;
    std::array<WireSeq, kCapacity> items{};
    uint8_t count = 0;  // valid entries, saturating at kCapacity
    uint8_t next = 0;   // write cursor

    void Add(WireSeq seq) {
      items[next] = seq;
      next = static_cast<uint8_t>((next + 1) % kCapacity);
      if (count < kCapacity) {
        count++;
      }
    }
    const WireSeq* begin() const { return items.data(); }
    const WireSeq* end() const { return items.data() + count; }
  };
  static TransmissionLog MakeLog(WireSeq seq);

  struct QueuedOut {
    std::shared_ptr<Buffer> payload;
    SendOptions options;
  };
  struct SentInfo {
    TimePoint send_time;
    bool reliable = false;
    MsgKey reliable_key;
  };
  // one outstanding reliable datagram (a whole small message, or one fragment of
  // a large one). Fragments of a message share the underlying `message` buffer.
  struct OutReliable {
    std::shared_ptr<Buffer> message;
    size_t offset = 0;
    size_t length = 0;
    uint8_t channel = 0;
    uint16_t message_seq = 0;
    uint8_t frag_index = 0;
    uint8_t frag_count = 1;
    uint8_t data_flags = 0;  // includes kFlagData and, if fragmented, kFlagFragment
    TimePoint last_send;
    int send_count = 0;
    // the packet_seqs this datagram has been sent under. Acking any one of them
    // retires the message and clears the rest from sent_packets_, so a lost
    // transmission cannot orphan a record there. Fixed-size and inline: no
    // allocation per message, and it cannot grow with retry count. Keeps the
    // most recent transmissions; older ones age out of sent_packets_ anyway.
    TransmissionLog packets;
  };
  struct Reassembly {
    uint8_t frag_count = 0;
    std::map<uint8_t, std::vector<uint8_t>> fragments;  // frag_index -> bytes
    TimePoint first_seen;
  };
  // Per-channel receive state. Reliable and unreliable traffic use separate
  // sequence spaces, so both can share a channel without interfering. Within the
  // reliable substream pick ordered XOR unordered; within the unreliable
  // substream pick sequenced XOR unordered.
  //   reliable + ordered    -> rel_expected (next) + rel_reorder (buffered)
  //   reliable + unordered  -> rel_expected (low watermark) + rel_delivered_ahead
  //   unreliable + ordered  -> unrel_last + unrel_started (sequenced drop-old)
  //   unreliable + unordered-> stateless (deliver every arrival)
  //
  // Allocated lazily per channel actually used, so idle channels cost nothing.
  struct ChannelState {
    // send
    SequenceId rel_send = 0;
    SequenceId unrel_send = 0;
    // receive
    SequenceId rel_expected = 0;
    std::map<SequenceId, std::shared_ptr<Buffer>> rel_reorder;
    std::set<SequenceId> rel_delivered_ahead;
    SequenceId unrel_last = 0;
    bool unrel_started = false;
  };

  std::shared_ptr<UDPSocket> socket_;
  std::shared_ptr<InetAddress> peer_;
  ZDTOptions config_;
  bool drains_own_socket_;
  std::shared_ptr<ZDTInbox> inbox_;
  ZDTConnection connection_;

  // packet_seq 0 is reserved as the "nothing to ack yet" sentinel: a peer that
  // has not received anything sends ack=0, and AckPacket(0) is a no-op. Without
  // this, the first datagram of each side falsely acks the peer's first packet
  // on a simultaneous open (both P2P punch peers, or a busy client/server).
  WireSeq next_packet_seq_ = 1;

  std::deque<std::shared_ptr<Buffer>> ready_;
  std::deque<QueuedOut> outbound_;
  bool is_closed_ = false;

  TimePoint last_recv_;
  TimePoint last_send_;

  // reliability: RTT/RTO (Jacobson/Karels).
  double srtt_ms_ = 0.0;
  double rttvar_ms_ = 0.0;
  bool has_rtt_ = false;
  std::chrono::milliseconds rto_{200};

  // reliability, sender: in-flight datagrams and unacked reliable datagrams.
  std::unordered_map<uint16_t, SentInfo> sent_packets_;
  std::map<MsgKey, OutReliable> unacked_;

  // reliability, receiver: ack state and per-channel ordered delivery.
  uint16_t remote_ack_seq_ = 0;
  uint32_t remote_ack_bits_ = 0;
  bool has_remote_seq_ = false;
  bool needs_ack_ = false;
  std::unordered_map<uint8_t, ChannelState> channels_;  // allocated on first use
#if ZNET_ENABLE_METRICS
  SessionMetrics metrics_;
#endif
  std::map<MsgKey, Reassembly> reassembly_;  // frag_index unused in this key
};

class ZDTClientBackend : public ClientBackend {
 public:
  explicit ZDTClientBackend(std::shared_ptr<InetAddress> server_address,
                            const SessionOptions& options = {});
  ~ZDTClientBackend() override;
  ZDTClientBackend(const ZDTClientBackend&) = delete;

  Result Bind() override;
  Result Bind(const std::string& ip, PortNumber port) override;
  Result Connect() override;
  Result Close() override;
  void Update() override;
  bool IsAlive() override;

  std::mutex& mutex() override { return mutex_; }
  std::shared_ptr<PeerSession> client_session() override { return client_session_; }
  std::shared_ptr<InetAddress> local_address() override { return local_address_; }

 private:
  // runs the synchronous RakNet-style handshake on socket_. On success fills
  // `out` with the negotiated connection and returns Result::Success; otherwise a
  // granular failure Result (IncompatibleVersion, ServerFull, Timeout, ...).
  Result Handshake(ZDTConnection& out);

  std::mutex mutex_;
  std::shared_ptr<InetAddress> server_address_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<UDPSocket> socket_;
  std::shared_ptr<PeerSession> client_session_;
  ZDTOptions config_;
  uint64_t guid_ = 0;
  bool is_bind_ = false;
};

class ZDTServerBackend : public ServerBackend {
 public:
  explicit ZDTServerBackend(std::shared_ptr<InetAddress> bind_address,
                            const SessionOptions& child_options = {});
  ~ZDTServerBackend() override;
  ZDTServerBackend(const ZDTServerBackend&) = delete;

  Result Bind() override;
  Result Listen() override;
  Result Close() override;
  void Update() override;

  std::shared_ptr<PeerSession> Accept() override;
  void AcceptAndReject() override;
  bool IsAlive() override;

  std::mutex& mutex() override { return mutex_; }

  std::shared_ptr<InetAddress> bind_address() const override {
    return bind_address_;
  }

  ServerMetrics metrics() const override {
    ServerMetrics out = metrics_;
    out.transport = ConnectionType::ZDT;
    return out;
  }

 private:
  struct Route {
    std::weak_ptr<PeerSession> session;
    std::shared_ptr<ZDTInbox> inbox;
    std::shared_ptr<InetAddress> peer;
    uint64_t remote_guid = 0;
  };

  // Drains all pending datagrams from the socket and routes them: online -> the
  // matching peer's inbox; offline -> the stateless handshake path (which may
  // create a session and push it onto pending_accept_). Called from Accept().
  void DrainAndRoute();
  void HandleOffline(Buffer& buffer, const std::shared_ptr<InetAddress>& from,
                     size_t datagram_size);
  void MaybeRotateSecret();
  ZDTCookie CookieFor(const std::string& peer_readable, uint32_t epoch) const;
  // Per-source handshake rate limit (bounded, self-pruning). Returns false when
  // the source has exceeded per_source_handshake_rate this second.
  bool AllowHandshake(const std::string& peer_readable);

  std::mutex mutex_;  // exposed lock the Server holds during a tick
  std::shared_ptr<InetAddress> bind_address_;
  std::shared_ptr<UDPSocket> socket_;
  ZDTOptions config_;
  bool is_bind_ = false;
  bool is_listening_ = false;

  struct SourceRate {
    int count = 0;
    std::chrono::steady_clock::time_point window_start;
  };

  std::unordered_map<std::string, Route> routes_;
  std::deque<std::shared_ptr<PeerSession>> pending_accept_;
  std::unordered_map<std::string, SourceRate> source_rate_;

  // cookie signing secrets (touched only on the Accept()/MainProcessor thread).
  std::array<uint8_t, 32> secret_current_{};
  std::array<uint8_t, 32> secret_previous_{};
  uint32_t epoch_ = 0;
  bool has_previous_secret_ = false;
  std::chrono::steady_clock::time_point last_rotation_;
  uint64_t server_guid_ = 0;
  ServerMetrics metrics_;
};

}  // namespace backends
}  // namespace znet

#endif  // ZNET_PARENT_ZDT_H
