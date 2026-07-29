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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace znet {
namespace backends {

/** @brief Protocol version, checked for strict equality during the handshake. */
ZNET_INLINE_CONSTEXPR uint8_t kZDTProtocolVersion = 2;

/**
 * @brief Prefix on offline (pre-connection) messages.
 *
 * Keeps unrelated UDP traffic on the port from being parsed as a handshake.
 */
ZNET_INLINE_CONSTEXPR std::array<uint8_t, 8> kZDTMagic = {'Z', 'N', 'E', 'T',
                                                     'Z', 'D', 'T', 0x01};

// online datagram flags (byte 0), connection-level only. bit 7 separates
// connected-state datagrams from offline handshake messages, which the demux
// keys on. Anything about an individual message lives in its record flags.
ZNET_INLINE_CONSTEXPR uint8_t kFlagFin = 1u << 0;     // graceful close
ZNET_INLINE_CONSTEXPR uint8_t kFlagPing = 1u << 1;    // keepalive probe
ZNET_INLINE_CONSTEXPR uint8_t kFlagPong = 1u << 2;    // keepalive reply
ZNET_INLINE_CONSTEXPR uint8_t kFlagOnline = 1u << 7;  // online-datagram marker

// per-record flags (byte 0 of each message record).
ZNET_INLINE_CONSTEXPR uint8_t kRecReliable = 1u << 0;  // retransmit until acked
ZNET_INLINE_CONSTEXPR uint8_t kRecOrdered = 1u << 1;   // ordering applies on channel
ZNET_INLINE_CONSTEXPR uint8_t kRecFragment = 1u << 2;  // frag_index/frag_count present

/** @brief Offline (handshake) message ids, all < 0x80 so they never set kFlagOnline. */
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

// a datagram is one header followed by zero or more message records, so small
// messages share one instead of each paying for its own. zero records is a valid
// control datagram (bare ack, ping, pong or fin).
// flags(1) + packet_seq(2) + ack(2) + block_count(1), then 2 bytes per block.
// this is the fixed part; ReadZDTHeader checks the blocks separately.
ZNET_INLINE_CONSTEXPR size_t kZDTHeaderSize = 6;
// rec_flags, channel, message_seq, length
ZNET_INLINE_CONSTEXPR size_t kZDTRecordHeaderSize = 6;
// the above plus frag_index and frag_count
ZNET_INLINE_CONSTEXPR size_t kZDTFragRecordHeaderSize = 8;

// sequences go on the wire truncated to 16 bits but are tracked in full: a
// truncated value aliases every 65536 messages, which would let a late retransmit
// reconstruct onto a live message and corrupt ordering and map keys.
using WireSeq = uint16_t;
using SequenceId = uint64_t;

// rebuilds the full SequenceId from a truncated wire value, picking the
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

// how far back the receiver remembers which packet_seqs arrived. the encoder
// walks this to build ack blocks, so it bounds what one acknowledgement can
// describe and therefore how large the send window may usefully grow.
ZNET_INLINE_CONSTEXPR size_t kZDTAckHistoryBits = 1024;
ZNET_INLINE_CONSTEXPR size_t kZDTAckHistoryWords = kZDTAckHistoryBits / 64;

// ack blocks per datagram. each is a run of received packets followed by the
// run of missing ones just older, walking backwards from `ack`. a gap that does
// not fit is described by a later acknowledgement, so this bounds header size
// rather than what can eventually be reported.
ZNET_INLINE_CONSTEXPR size_t kZDTMaxAckBlocks = 24;
// each block is num_ack(1) + num_nack(1)
ZNET_INLINE_CONSTEXPR size_t kZDTAckBlockSize = 2;
// blocks a data datagram always has room for. kZDTHeaderSize covers only the
// fixed part, so records are packed against this larger figure and the ack
// encoder is capped at whatever is actually left. without the reserve a full
// datagram would carry no ack at all.
ZNET_INLINE_CONSTEXPR size_t kZDTAckBlocksReserved = 4;
ZNET_INLINE_CONSTEXPR size_t kZDTHeaderReserve =
    kZDTHeaderSize + kZDTAckBlocksReserved * kZDTAckBlockSize;

// how long a base round trip measurement stays authoritative. the minimum is
// the queue-free path, so it has to be re-probed: a route change or a handover
// raises the floor permanently, and a minimum kept for the whole connection
// would read the new baseline as congestion and never open the window again.
ZNET_INLINE_CONSTEXPR int kZDTRttMinWindowMs = 10000;

// round trip inflation that counts as a queue building rather than jitter, and
// how hard to back off when it does.
ZNET_INLINE_CONSTEXPR double kZDTQueueingRttRatio = 1.25;
ZNET_INLINE_CONSTEXPR double kZDTQueueingBackoff = 0.85;

ZNET_INLINE_CONSTEXPR int kZDTMaxDatagramsInFlight =
    static_cast<int>(kZDTAckHistoryBits);

// one run of received packets and the run of missing ones immediately older.
// blocks are ordered newest first, the first one ending at `ack`.
struct ZDTAckBlock {
  uint8_t num_ack = 0;   // consecutive received, ending at this block's head
  uint8_t num_nack = 0;  // consecutive missing, just older than those
};

struct ZDTHeader {
  uint8_t flags = kFlagOnline;
  uint16_t packet_seq = 0;  // connection-level, ++ per datagram (drives ack/RTT)
  uint16_t ack = 0;         // highest packet_seq seen from peer
  // run-length encoded picture of what arrived, walking back from `ack`. the
  // nack runs are the negative acknowledgement, so nothing caps the window at
  // what fits in a fixed-width bitfield.
  std::array<ZDTAckBlock, kZDTMaxAckBlocks> blocks{};
  uint8_t block_count = 0;
};

// one message, or one fragment of one, inside a datagram.
struct ZDTRecord {
  uint8_t flags = 0;         // kRec*
  uint8_t channel = 0;
  uint16_t message_seq = 0;  // per-channel message sequence
  uint8_t frag_index = 0;
  uint8_t frag_count = 1;
  uint16_t length = 0;  // payload bytes following this record header
};

// serializes `header` (big-endian) to the front of `buffer`.
void WriteZDTHeader(Buffer& buffer, const ZDTHeader& header);
// parses an online header from the front of `buffer`; returns false if the buffer
// is too short or does not carry the online marker. leaves the read cursor at the
// first record on success.
bool ReadZDTHeader(Buffer& buffer, ZDTHeader& out_header);

// record header only; the payload follows and is not copied.
void WriteZDTRecord(Buffer& buffer, const ZDTRecord& record);
// reads a record header and validates that `length` bytes actually follow.
// leaves the read cursor at the record's payload.
bool ReadZDTRecord(Buffer& buffer, ZDTRecord& out_record);

// bytes a record occupies on the wire, header plus payload.
inline size_t ZDTRecordSize(bool fragment, size_t payload_len) {
  return (fragment ? kZDTFragRecordHeaderSize : kZDTRecordHeaderSize) +
         payload_len;
}

// writes an offline message id followed by kZDTMagic.
void WriteOfflineHeader(Buffer& buffer, ZDTOfflineMsg id);
// reads and validates an offline header (id < 0x80 and correct magic). on success
// the read cursor is left just past the magic and `out_id` holds the message id.
bool ReadOfflineHeader(Buffer& buffer, ZDTOfflineMsg& out_id);

// --- Return-routability cookie ------------------------------------------------
// server issues HMAC(secret[epoch], addr, epoch) in Reply1 holding no state, and
// only allocates a session once the client echoes it back in Request2.
ZNET_INLINE_CONSTEXPR size_t kZDTCookieLen = 16;
using ZDTCookie = std::array<uint8_t, kZDTCookieLen>;

ZDTCookie ComputeCookie(const uint8_t* secret, size_t secret_len,
                        const std::string& peer_readable, uint32_t epoch);
// constant-time comparison (no early-out) to avoid timing side channels.
bool ConstTimeEqual(const ZDTCookie& a, const ZDTCookie& b);
// 64-bit random peer identifier (OpenSSL RAND_bytes).
uint64_t GenerateGuid();

enum class RecvResult { Received, WouldBlock, Error };

// thin owner of a UDP socket. shared by a server's per-peer transports: concurrent
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
  // headroom for bursts that arrive between drains. Best-effort: the kernel
  // clamps to its own maximum and reports no error when it does.
  bool SetReceiveBufferSize(int bytes);
  // best-effort; the handshake MTU probe needs oversized datagrams dropped
  // rather than IP-fragmented.
  bool SetDontFragment(bool enabled);

  /**
   * @brief Wakes a blocked RecvFrom without releasing the descriptor.
   *
   * For tearing down a socket another thread may be reading. Close() returns
   * the descriptor number to the OS, where the next socket or file opened
   * anywhere in the process can reuse it while that read is still running on
   * it; the destructor closes it once every holder is gone.
   */
  bool Shutdown();

  Result Close();
  bool IsValid() const { return IsValidSocketHandle(socket_); }
  SocketHandle handle() const { return socket_; }
  std::shared_ptr<InetAddress> local_address();

 private:
  SocketHandle socket_ = kSocketInvalid;
};

// thread-safe raw-datagram queue, shared by a producer and a consumer running on
// different threads (see ZDTTransportLayer for the threading rule).
class ZDTInbox {
 public:
  // drops and returns false once `limit` datagrams are pending, so a flooding
  // peer cannot grow this queue without bound.
  bool Push(const uint8_t* data, size_t len, size_t limit);
  void Drain(std::deque<std::vector<uint8_t>>& out);
  size_t dropped() const;

 private:
  mutable std::mutex mutex_;
  std::deque<std::vector<uint8_t>> queue_;
  size_t dropped_ = 0;
};

// per-connection parameters settled by the handshake.
struct ZDTConnection {
  uint16_t mtu = 1200;
  uint64_t local_guid = 0;
  uint64_t remote_guid = 0;
};

// per-peer transport. all protocol state is touched only on the owning session's
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
  void Flush() override;

  // feeds one raw ZDT datagram (UDP payload) to this transport. Thread-safe.
  void OnDatagram(const uint8_t* data, size_t len);

  void FillMetrics(SessionMetrics& out) const override;

  std::shared_ptr<InetAddress> peer() const { return peer_; }
  std::shared_ptr<ZDTInbox> inbox() const { return inbox_; }

 private:
  using TimePoint = std::chrono::steady_clock::time_point;

  // identifies one reliable datagram / reassembly buffer.
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

  // one message queued for the datagram being packed. `owner` keeps the source
  // buffer alive until the batch goes out, since `payload` points into it.
  struct PendingRecord {
    ZDTRecord record;
    std::shared_ptr<Buffer> owner;
    const char* payload = nullptr;
    size_t payload_len = 0;
    bool reliable = false;
    MsgKey key;
  };

  // assigns a fresh packet_seq, piggybacks the current ack and writes every
  // record into one datagram. `extra_flags` excludes kFlagOnline. Returns the
  // packet_seq, which callers log against each reliable message they added.
  WireSeq SendBatch(uint8_t extra_flags, const PendingRecord* batch,
                    size_t count);

  // encodes the arrival history into at most max_blocks blocks, so the caller
  // can hold the datagram inside the MTU.
  void FillAck(ZDTHeader& header, size_t max_blocks);
  // congestion control: grows while acks arrive, backs off on queueing delay.
  void OnCongestionAck(int acked_datagrams);
  ZNET_NODISCARD int SendWindow() const;
  ZNET_NODISCARD int SendWindowCap() const;
  // marks a reported gap for immediate retransmit and stops tracking it, so one
  // loss costs one resend however often the peer keeps reporting it.
  void OnNak(WireSeq packet_seq);
  void RecordRemoteSeq(WireSeq packet_seq);   // fold an arrival into our history
  void ProcessAcks(const ZDTHeader& header);  // consume the peer's ack blocks
  bool AckPacket(WireSeq packet_seq);  // true if this ack was new
  void UpdateRtt(std::chrono::steady_clock::duration sample, TimePoint now);
  void RetransmitUnacked();
  void PruneSentPackets();
  // expands the record's truncated message_seq to a full SequenceId, using the
  // matching substream's position on that channel as context.
  SequenceId ReconstructSeqFor(const ZDTRecord& record);
  // return false when the record could not be taken (reassembly at its limit).
  // the caller then leaves the datagram unacked so the sender retransmits,
  // rather than taking responsibility for data it dropped.
  bool OnRecord(const ZDTRecord& record, const uint8_t* data, size_t len);
  bool OnDataFragment(const ZDTRecord& record, const uint8_t* data, size_t len);
  void PruneReassembly();
  void DeliverMessage(const ZDTRecord& record, std::shared_ptr<Buffer> payload);

  // ring of the most recent packet_seqs a reliable datagram was sent under.
  // fixed capacity, stored inline, so tracking retransmissions never allocates
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

  // describes one message, or one fragment of one, for the send path. `offset`
  // is into `owner`, which stays alive until the batch goes out.
  static PendingRecord MakeRecord(const std::shared_ptr<Buffer>& owner,
                                  size_t offset, size_t length, uint8_t flags,
                                  uint8_t channel, SequenceId message_seq,
                                  uint8_t frag_index, uint8_t frag_count,
                                  bool reliable);
  // files a reliable record under its key so it retransmits until acked. `log`
  // carries the packet_seq when known; the batching path fills it in later.
  void TrackReliable(const PendingRecord& pending, size_t offset, TimePoint now,
                     TransmissionLog log);

  struct QueuedOut {
    std::shared_ptr<Buffer> payload;
    SendOptions options;
  };
  // a datagram can carry several reliable messages, so acking it retires all of
  // them. fixed capacity and inline, like TransmissionLog: batching must not add
  // an allocation per datagram.
  struct SentInfo {
    static constexpr size_t kMaxKeys = 64;
    TimePoint send_time;
    std::array<MsgKey, kMaxKeys> keys{};
    uint8_t key_count = 0;  // reliable messages in this datagram

    void Add(const MsgKey& key) {
      if (key_count < kMaxKeys) {
        keys[key_count++] = key;
      }
    }
    const MsgKey* begin() const { return keys.data(); }
    const MsgKey* end() const { return keys.data() + key_count; }
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
  // per-channel receive state. reliable and unreliable traffic use separate
  // sequence spaces, so both can share a channel without interfering. Within the
  // reliable substream pick ordered XOR unordered; within the unreliable
  // substream pick sequenced XOR unordered.
  //   reliable + ordered    -> rel_expected (next) + rel_reorder (buffered)
  //   reliable + unordered  -> rel_expected (low watermark) + rel_delivered_ahead
  //   unreliable + ordered  -> unrel_last + unrel_started (sequenced drop-old)
  //   unreliable + unordered-> stateless (deliver every arrival)
  //
  // allocated lazily per channel actually used, so idle channels cost nothing.
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
  // Send() may be called from any thread; FlushOutbound() drains on the
  // session's worker. The lock covers only the queue, never a sendto().
  mutable std::mutex outbound_mutex_;
  std::deque<QueuedOut> outbound_;
  // messages queued but not yet handed to the wire, counting both outbound_ and
  // staged_. Send() bounds itself on this rather than outbound_.size(), which
  // reads short whenever staged_ is holding some.
  size_t outbound_count_ = 0;
  // reused across ProcessInbound() calls. a default-constructed std::deque
  // allocates its map and first node immediately, so declaring this local meant
  // two mallocs on every tick whether or not a datagram had arrived. swapping
  // into a member keeps those nodes alive between calls. session worker only.
  std::deque<std::vector<uint8_t>> inbound_scratch_;
  // the oldest queued messages, already taken off outbound_ but not yet sent
  // because the send window refused them. holding them here instead of putting
  // them back means FlushOutbound() takes the lock once per refill rather than
  // once per message, and never has to splice a partial batch back onto the
  // front of a deque. touched only by the session worker, so it needs no lock.
  std::deque<QueuedOut> staged_;
  // read via IsAlive() from whichever thread owns the application, written by
  // Close() from the same, so it cannot be a plain bool
  std::atomic_bool is_closed_{false};

  TimePoint last_recv_;
  TimePoint last_send_;

  // reliability: RTT/RTO (Jacobson/Karels).
  double srtt_ms_ = 0.0;
  double rttvar_ms_ = 0.0;
  // lowest round trip seen, i.e. the path with no queue in it. congestion is
  // judged against this rather than against packet loss: netem-style random
  // loss, and the wireless loss it stands in for, is not congestion, and
  // halving the window for it is how a loss-based controller strangles itself
  // on a lossy link.
  double rtt_min_ms_ = 0.0;
  // when rtt_min_ms_ was last set, so it can be re-probed. see
  // kZDTRttMinWindowMs.
  TimePoint rtt_min_stamp_;
  bool has_rtt_ = false;
  std::chrono::milliseconds rto_{200};

  // reliability, sender: in-flight datagrams and unacked reliable datagrams.
  std::unordered_map<uint16_t, SentInfo> sent_packets_;
  std::map<MsgKey, OutReliable> unacked_;
  // soonest moment any unacked message can fall due; before it, the retransmit
  // scan has nothing to find and is skipped entirely
  TimePoint next_retransmit_scan_ = TimePoint::min();

  // reliability, receiver: ack state and per-channel ordered delivery.
  uint16_t remote_ack_seq_ = 0;
  // bit i means "packet_seq (remote_ack_seq_ - i) arrived"; bit 0 is
  // remote_ack_seq_ itself and is always set once has_remote_seq_ is true.
  std::array<uint64_t, kZDTAckHistoryWords> remote_ack_bits_{};
  // how much of remote_ack_bits_ describes sequences the peer has actually
  // sent. the rest is history never observed, and encoding it as missing would
  // invent losses out of a freshly opened connection.
  size_t history_valid_bits_ = 0;
  // congestion window in datagrams, and the slow-start threshold it switches
  // to congestion avoidance at. a double so congestion avoidance can grow it by
  // a fraction of a datagram per ack rather than rounding to nothing.
  double cwnd_ = 10.0;       // initial window, TCP's IW10
  double ssthresh_ = 1e9;    // no threshold until the first loss teaches one
  // suppresses repeated halving inside one round trip: one loss event should
  // cost one halving, not one per lost datagram in the same window.
  WireSeq loss_recovery_until_ = 0;
  bool in_loss_recovery_ = false;
  bool has_remote_seq_ = false;
  bool needs_ack_ = false;
  std::unordered_map<uint8_t, ChannelState> channels_;  // allocated on first use
#if ZNET_ENABLE_METRICS
  SessionMetrics metrics_;
#endif
  std::map<MsgKey, Reassembly> reassembly_;  // frag_index unused in this key
  // bytes currently held in reassembly_. tracked so the caps can act as
  // backpressure (refuse to start new messages) instead of discarding data.
  size_t reassembly_bytes_ = 0;
};

class ZDTClientBackend : public ClientBackend {
 public:
  explicit ZDTClientBackend(std::shared_ptr<InetAddress> server_address,
                            const SessionOptions& options = {});
  ~ZDTClientBackend() override;
  ZDTClientBackend(const ZDTClientBackend&) = delete;

  Result Bind() override;
  Result Bind(const std::string& ip, PortNumber port) override;

 private:
  // shared by both Bind() overloads: open, configure, bind, record the address
  Result BindTo(const InetAddress& address);

 public:
  Result Connect() override;
  Result Close() override;
  void Update() override;
  bool IsAlive() override;
  void SetWakeCallback(std::function<void()> on_data) override {
    on_data_ = std::move(on_data);
  }
  void StopReceiving() override;
  bool DrivesOwnReceive() const override { return true; }

  std::mutex& mutex() override { return mutex_; }
  std::shared_ptr<PeerSession> client_session() override { return client_session_; }
  std::shared_ptr<InetAddress> local_address() override { return local_address_; }

 private:
  // on success fills `out` with the negotiated connection and returns Result::Success; otherwise a
  // granular failure Result (IncompatibleVersion, ServerFull, Timeout, ...).
  Result Handshake(ZDTConnection& out);

  // like the server's, so an arriving datagram is seen at once rather than on
  // the client loop's next tick. started only after the handshake, which reads
  // the socket directly and would otherwise race it.
  void ReceiveLoop();

  std::mutex mutex_;
  std::shared_ptr<InetAddress> server_address_;
  std::shared_ptr<InetAddress> local_address_;
  std::shared_ptr<UDPSocket> socket_;
  std::shared_ptr<ZDTInbox> inbox_;
  std::thread receive_thread_;
  std::mutex receive_thread_mutex_;
  std::atomic_bool receiving_{false};
  std::function<void()> on_data_;
  std::shared_ptr<PeerSession> client_session_;
  ZDTOptions config_;
  SessionOptions session_options_;  // passed to the PeerSession it creates
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

  void SetWakeCallback(std::function<void()> on_data) override {
    on_data_ = std::move(on_data);
  }

  void StopReceiving() override;

  std::shared_ptr<InetAddress> bind_address() const override {
    return bind_address_;
  }

  ServerMetrics metrics() const override {
    // the receive thread writes these counters, so sample under the lock
    std::lock_guard<std::mutex> lock(state_mutex_);
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

  // body of the receive thread: blocks in recvfrom and routes each datagram as
  // it lands. Online -> the matching peer's inbox; offline -> the stateless
  // handshake path (which may create a session and push it onto
  // pending_accept_). Returns when is_listening_ goes false.
  void ReceiveLoop();
  void RouteDatagram(uint8_t* data, size_t len,
                     const std::shared_ptr<InetAddress>& from);
  void HandleOffline(Buffer& buffer, const std::shared_ptr<InetAddress>& from,
                     size_t datagram_size);
  void MaybeRotateSecret();
  ZDTCookie CookieFor(const std::string& peer_readable, uint32_t epoch) const;
  // per-source handshake rate limit (bounded, self-pruning). returns false when
  // the source has exceeded per_source_handshake_rate this second.
  bool AllowHandshake(const std::string& peer_readable);

  std::mutex mutex_;  // exposed lock the Server holds during a tick
  std::shared_ptr<InetAddress> bind_address_;
  std::shared_ptr<UDPSocket> socket_;
  ZDTOptions config_;
  SessionOptions child_session_options_;  // passed to each accepted PeerSession
  std::atomic_bool is_bind_{false};
  std::atomic_bool is_listening_{false};

  struct SourceRate {
    int count = 0;
    std::chrono::steady_clock::time_point window_start;
  };

  // routes_, pending_accept_, source_rate_, the cookie secrets and metrics_ are
  // written by the receive thread and read by the Server's tick, so they need a
  // lock. Deliberately not mutex_: the Server holds that across a whole tick,
  // and stalling the receive thread that long is what overflows the socket.
  mutable std::mutex state_mutex_;
  // set once before the receive thread starts and never reassigned, so the
  // thread can read it without synchronizing.
  std::function<void()> on_data_;
  // StopReceiving() is reachable both from the shutdown path and from Close()
  // on another thread. Joining the same thread twice is undefined, so entry is
  // serialized here.
  std::mutex receive_thread_mutex_;
  std::thread receive_thread_;
  std::atomic_bool receiving_{false};

  std::unordered_map<std::string, Route> routes_;
  std::deque<std::shared_ptr<PeerSession>> pending_accept_;
  std::unordered_map<std::string, SourceRate> source_rate_;

  // cookie signing secrets (touched only on the receive thread).
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
