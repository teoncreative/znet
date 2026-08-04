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
// The per-peer ZDT transport: reliability, congestion control, fragmentation,
// ordering and keepalive for one connection.
//

#ifndef ZNET_BACKENDS_ZDT_ZDT_TRANSPORT_H_
#define ZNET_BACKENDS_ZDT_ZDT_TRANSPORT_H_

#include "znet/backends/backend.h"
#include "znet/buffer.h"
#include "znet/inet_addr.h"
#include "znet/metrics.h"
#include "znet/mpsc_queue.h"
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
#include "znet/backends/zdt/zdt_ack_history.h"
#include "znet/backends/zdt/zdt_congestion.h"
#include "znet/backends/zdt/zdt_connection.h"
#include "znet/backends/zdt/zdt_domain.h"
#include "znet/backends/zdt/zdt_net.h"
#include "znet/backends/zdt/zdt_wire.h"

namespace znet {
namespace backends {


// per-peer transport. all protocol state is touched only on the owning session's
// worker thread (Update/Receive/Send). OnDatagram may be called from any thread;
// it just appends raw bytes to the inbox which Update() drains.
class ZDTTransportLayer : public TransportLayer {
 public:
  // `common` carries the transport-agnostic keepalive knobs; the defaults
  // match CommonOptions so call sites without a SessionOptions in hand (the
  // dialer, the tests) behave like a default session
  ZDTTransportLayer(std::shared_ptr<UDPSocket> socket,
                    std::shared_ptr<InetAddress> peer, ZDTOptions config,
                    bool drains_own_socket, std::shared_ptr<ZDTInbox> inbox,
                    ZDTConnection connection,
                    CommonOptions common = CommonOptions());
  ~ZDTTransportLayer() override;

  std::shared_ptr<Buffer> Receive() override;
  bool Send(std::shared_ptr<Buffer> buffer, SendOptions options = {}) override;

  /** @brief ZDT orders each channel on its own, so the channel is the domain. */
  uint8_t OrderingDomain(const SendOptions& options) const override {
    return options.GetOr<ChannelKey>(0);
  }
  Result Close(CloseOptions options = {}) override;
  bool IsClosed() const override { return is_closed_; }
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
  size_t StageOutbound(); // move ring entries into their channel lanes
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
  // congestion control: grows while acks arrive, backs off on queueing delay.
  /** @brief Whether the round trip says a real queue is building. */
  ZNET_NODISCARD int SendWindow() const;
  ZNET_NODISCARD int SendWindowCap() const;
  // marks a reported gap for immediate retransmit and stops tracking it, so one
  // loss costs one resend however often the peer keeps reporting it.
  void OnNak(WireSeq packet_seq);
  void ProcessAcks(const ZDTHeader& header);  // consume the peer's ack blocks
  bool AckPacket(WireSeq packet_seq);  // true if this ack was new
  void RetransmitUnacked();
  // resends the newest unacked message when acks go silent, so a lost burst
  // tail does not wait out the RTO floor. Even when the probed message was not
  // the one lost, its fresh packet_seq advances the peer's ack horizon and the
  // real gap comes back as a NAK. See kZDTTailProbeFloorMs.
  void MaybeTailProbe(TimePoint now);
  std::chrono::steady_clock::duration TailProbeDelay() const;
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
    // every packet_seq this datagram went out under. acking any one retires the
    // message and clears the rest, so a lost transmission cannot orphan a
    // sent_packets_ record. fixed-size, so retries cannot grow it; the oldest
    // fall off and age out of sent_packets_ on their own.
    TransmissionLog packets;
  };
  struct Reassembly {
    uint8_t frag_count = 0;
    std::map<uint8_t, std::vector<uint8_t>> fragments;  // frag_index -> bytes
    TimePoint first_seen;
  };
  // per-channel state. reliable and unreliable keep separate sequence spaces,
  // so one channel carries both without interference. which members are live
  // depends on the delivery mode:
  //   reliable + ordered    -> rel_expected (next) + rel_reorder (buffered)
  //   reliable + unordered  -> rel_expected (low watermark) + rel_delivered_ahead
  //   unreliable + ordered  -> unrel_last + unrel_started (sequenced drop-old)
  //   unreliable + unordered-> stateless (deliver every arrival)
  // allocated lazily, so idle channels cost nothing.
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
  // Send() runs on whichever thread is encoding the session, FlushOutbound()
  // on the owning worker, so the hand-off is lock-free.
  MpscQueue<QueuedOut> outbound_;
  // what the send window had no room for, one lane per channel so a stalled
  // or backlogged channel cannot hold another channel's traffic behind it.
  // Worker only, so it needs no lock. Lanes persist once created: a channel
  // that bursts repeatedly reuses its deque's nodes instead of allocating.
  struct StagedLane {
    uint8_t channel = 0;
    std::deque<QueuedOut> messages;
  };
  std::vector<StagedLane> staged_;
  size_t staged_count_ = 0;   // total queued across lanes; bounds the drain
  size_t staged_cursor_ = 0;  // rotates so no lane is always served first
  // reused across ProcessInbound() calls. a default-constructed std::deque
  // allocates its map and first node immediately, so declaring this local meant
  // two mallocs on every tick whether or not a datagram had arrived. swapping
  // into a member keeps those nodes alive between calls. session worker only.
  std::deque<std::vector<uint8_t>> inbound_scratch_;
  // read via IsAlive() from whichever thread owns the application, written by
  // Close() from the same, so it cannot be a plain bool
  std::atomic_bool is_closed_{false};
#ifndef NDEBUG
  // Update/Flush/Receive belong to whichever thread is driving this session.
  // Send, OnDatagram and Close are deliberately outside it: they are the three
  // entry points other threads are allowed to use.
  ThreadDomain worker_domain_;
#endif

  // the transport-agnostic keepalive knobs, from CommonOptions
  std::chrono::milliseconds keepalive_interval_;
  std::chrono::milliseconds idle_timeout_;
  TimePoint last_recv_;
  TimePoint last_send_;

  // reliability: RTT/RTO (Jacobson/Karels).
  // round trip estimate and the queueing signal derived from it
  ZDTRttEstimator rtt_;

  // reliability, sender: in-flight datagrams and unacked reliable datagrams.
  std::unordered_map<uint16_t, SentInfo> sent_packets_;
  // datagrams awaiting an acknowledgment, which is what the congestion window
  // bounds. not sent_packets_.size(): that also holds datagrams carrying no
  // reliable record, which the peer never acks individually, so they linger
  // until they age out and would otherwise hold window the whole time. On a
  // path whose window sits near the floor, a couple of keepalives were enough
  // to stall every reliable send until the next one happened to be acked.
  size_t in_flight_datagrams_ = 0;

  // Erases a sent_packets_ entry, keeping in_flight_datagrams_ true.
  void RetireSentPacket(std::unordered_map<uint16_t, SentInfo>::iterator it);
  // Same as above, by sequence; does nothing when the entry is already gone.
  void RetireSentPacket(uint16_t packet_seq);

  std::map<MsgKey, OutReliable> unacked_;
  // soonest moment any unacked message can fall due; before it, the retransmit
  // scan has nothing to find and is skipped entirely
  TimePoint next_retransmit_scan_ = TimePoint::min();
  // when the next tail-loss probe fires. Armed while reliable data is
  // outstanding; sends and ack progress push it back, each fire doubles it.
  TimePoint tail_probe_at_ = TimePoint::max();
  int tail_probes_fired_ = 0;

  // reliability, receiver: ack state and per-channel ordered delivery.
  // what arrived from the peer, and the ack blocks built from it
  ZDTAckHistory ack_history_;
  // the send window and its loss-epoch bookkeeping
  ZDTCongestionController congestion_;
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

}  // namespace backends
}  // namespace znet


#endif  // ZNET_BACKENDS_ZDT_ZDT_TRANSPORT_H_
