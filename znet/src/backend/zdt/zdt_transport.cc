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
// ZDT (znet Datagram Transport). See znet/backends/zdt.h for the overview.
//

#include "znet/backends/zdt/zdt_transport.h"

#include "znet/error.h"
#include "znet/logger.h"
#include "znet/util.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace znet {
namespace backends {

using steady_clock = std::chrono::steady_clock;


ZDTTransportLayer::TransmissionLog ZDTTransportLayer::MakeLog(WireSeq seq) {
  TransmissionLog log;
  log.Add(seq);
  return log;
}
// ---------------------------------------------------------------------------
// ZDTTransportLayer
// ---------------------------------------------------------------------------

ZDTTransportLayer::ZDTTransportLayer(std::shared_ptr<UDPSocket> socket,
                                     std::shared_ptr<InetAddress> peer,
                                     ZDTOptions config, bool drains_own_socket,
                                     std::shared_ptr<ZDTInbox> inbox,
                                     ZDTConnection connection,
                                     CommonOptions common)
    : socket_(std::move(socket)),
      peer_(std::move(peer)),
      config_(std::move(config)),
      drains_own_socket_(drains_own_socket),
      inbox_(inbox ? std::move(inbox) : std::make_shared<ZDTInbox>()),
      connection_(connection),
      // config_, not the parameter, which has been moved from by this point
      outbound_(config_.outbound_queue_capacity),
      keepalive_interval_(common.keepalive_interval),
      idle_timeout_(common.idle_timeout),
      last_recv_(steady_clock::now()),
      last_send_(steady_clock::now()) {
  rtt_.Reset(compat::Clamp(std::chrono::milliseconds(200), config_.rto_min,
                           config_.rto_max));
}

ZDTTransportLayer::~ZDTTransportLayer() {
  Close();
}

std::shared_ptr<Buffer> ZDTTransportLayer::Receive() {
  ZNET_ZDT_ENTER_DOMAIN(worker_domain_);
  if (ready_.empty()) {
    return nullptr;
  }
  auto buffer = ready_.front();
  ready_.pop_front();
  return buffer;
}

bool ZDTTransportLayer::Send(std::shared_ptr<Buffer> buffer, SendOptions options) {
  if (is_closed_) {
    ZNET_LOG_WARN("ZDT: tried to send on a closed transport, dropping packet!");
    return false;
  }
  // normally the session refuses long before this; this bounds what a shut
  // send window can accumulate over many ticks. No out-count: FlushOutbound()
  // runs on this transport's own worker, so there is nothing to wake.
  if (!outbound_.Push(QueuedOut{std::move(buffer), options})) {
    ZNET_LOG_WARN("ZDT: outbound queue full ({}), dropping packet!",
                  outbound_.capacity());
    return false;
  }
  return true;
}

void ZDTTransportLayer::OnDatagram(const uint8_t* data, size_t len) {
  if (!inbox_->Push(data, len, config_.max_inbox_datagrams)) {
    ZNET_METRIC(metrics_.zdt.inbound_dropped++);
  }
}

void ZDTTransportLayer::FillMetrics(SessionMetrics& out) const {
#if ZNET_ENABLE_METRICS
  out.zdt = metrics_.zdt;
  out.zdt.inbound_dropped += inbox_->dropped();
  out.common.wire_bytes_sent = metrics_.common.wire_bytes_sent;
  out.common.wire_bytes_received = metrics_.common.wire_bytes_received;
  // live state, sampled at call time
  out.zdt.srtt_us = static_cast<uint32_t>(rtt_.srtt_ms() * 1000.0);
  out.zdt.rtt_min_us = static_cast<uint32_t>(rtt_.rtt_min_ms() * 1000.0);
  out.zdt.rto_us = static_cast<uint32_t>(rtt_.rto().count() * 1000);
  out.zdt.cwnd = static_cast<uint32_t>(congestion_.cwnd());
  // datagrams, not messages: this is the quantity the congestion window bounds
  // and the one worth reading against cwnd and max_datagrams_in_flight.
  // Coalescing puts many messages in one datagram, so unacked_.size() is a
  // different and much larger number.
  out.zdt.in_flight = static_cast<uint32_t>(in_flight_datagrams_);
  out.zdt.mtu = connection_.mtu;
#else
  (void)out;
#endif
}

void ZDTTransportLayer::DrainSocket() {
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  while (true) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    RecvResult result = socket_->RecvFrom(buffer, sizeof(buffer), len, from);
    if (result != RecvResult::Received) {
      break;
    }
    inbox_->Push(buffer, len, config_.max_inbox_datagrams);
  }
}

void ZDTTransportLayer::ProcessInbound() {
  // cleared here, not after the loop: the FIN path returns early and Drain()
  // swaps, so leftovers would land back in the inbox and be processed twice.
  inbound_scratch_.clear();
  inbox_->Drain(inbound_scratch_);
  for (auto& raw : inbound_scratch_) {
    if (raw.empty() || !(raw[0] & kFlagOnline)) {
      continue;  // stray/offline datagram on a connected transport
    }
    Buffer buffer(reinterpret_cast<const char*>(raw.data()), raw.size(),
                  Endianness::BigEndian);
    ZDTHeader header;
    if (!ReadZDTHeader(buffer, header)) {
      continue;
    }
    last_recv_ = steady_clock::now();
    ZNET_METRIC(metrics_.zdt.datagrams_received++);
    ZNET_METRIC(metrics_.common.wire_bytes_received += raw.size());
    // packet_seq 0 is the sentinel used by stateless control datagrams (the
    // FIN a closing peer sends from its own thread); it carries no sequence or
    // ack state to fold in. The peer's acks are always consumed, but ours is
    // only extended to cover this datagram once every record in it was taken.
    if (header.packet_seq != 0) {
      ProcessAcks(header);
    }
    if (header.flags & kFlagFin) {
      is_closed_ = true;
      if (drains_own_socket_ && socket_) {
        // shut down rather than close, as Close() does: the application may be
        // in SendTo() on this socket, sending its own FIN
        socket_->Shutdown();
      }
      return;
    }
    if (header.flags & kFlagPing) {
      SendControl(kFlagPong);
      continue;
    }
    // everything after the header is a run of records. a datagram with none is
    // a bare ack, pong or keepalive, already handled above.
    bool accepted_all = true;
    while (buffer.readable_bytes() > 0) {
      ZDTRecord record;
      if (!ReadZDTRecord(buffer, record)) {
        ZNET_LOG_WARN("ZDT: malformed record from {}, dropping the rest.",
                      peer_->readable());
        break;
      }
      needs_ack_ = true;  // we owe the sender an ack for this message
      const char* payload = buffer.read_cursor_data();
      buffer.SkipRead(record.length);
      if (!OnRecord(record, reinterpret_cast<const uint8_t*>(payload),
                    record.length)) {
        accepted_all = false;
      }
      if (is_closed_) {
        break;
      }
    }
    // acknowledging a datagram tells the sender it may retire everything in it,
    // so only do that once all of it was taken. A refused datagram simply goes
    // unacked and the sender resends it after its RTO.
    if (header.packet_seq != 0 && accepted_all) {
      ack_history_.Record(header.packet_seq);
    }
  }
}

bool ZDTTransportLayer::OnRecord(const ZDTRecord& record, const uint8_t* data,
                                 size_t len) {
  if (record.flags & kRecFragment) {
    return OnDataFragment(record, data, len);
  }
  DeliverMessage(record,
                 std::make_shared<Buffer>(reinterpret_cast<const char*>(data),
                                          len));
  return true;
}

void ZDTTransportLayer::FlushOutbound() {
  uint16_t mtu = connection_.mtu != 0
                     ? connection_.mtu
                     : ZDTPayloadForLinkMTU(config_.mtu_ladder.back(),
                                            peer_->ipv());
  const size_t floor = kZDTHeaderReserve + kZDTFragRecordHeaderSize + 1;
  if (mtu < floor) {
    mtu = static_cast<uint16_t>(floor);
  }
  // largest payload that still fits one datagram as a single record. budgeted
  // against kZDTHeaderReserve, not kZDTHeaderSize: the ack blocks are part of
  // the header and a full datagram would otherwise overrun the MTU by however
  // many the encoder emitted.
  const size_t unfrag_capacity = mtu - kZDTHeaderReserve - kZDTRecordHeaderSize;
  const size_t frag_capacity = mtu - kZDTHeaderReserve - kZDTFragRecordHeaderSize;

  // pack into as few datagrams as the MTU allows rather than one each
  std::vector<PendingRecord>& batch = batch_scratch_;
  batch.clear();
  batch.reserve(SentInfo::kMaxKeys);
  size_t batch_bytes = kZDTHeaderReserve;
  auto flush_batch = [&]() {
    if (batch.empty()) {
      return;
    }
    WireSeq packet = SendBatch(0, batch.data(), batch.size());
    for (const PendingRecord& pending : batch) {
      if (!pending.reliable) {
        continue;
      }
      auto it = unacked_.find(pending.key);
      if (it != unacked_.end()) {
        it->second.packets.Add(packet);
      }
    }
    batch.clear();
    batch_bytes = kZDTHeaderReserve;
  };

  // bound the gap between the newest send and the oldest unacked message to half
  // a wire period, so a late retransmit can never reconstruct onto a live message.
  // cwnd normally keeps the gap far below this.
  constexpr SequenceId kMaxSeqGap = (SequenceId{1} << 16) / 2;
  const TimePoint now = steady_clock::now();

  // packs one message: batched when it fits a shared datagram, split into
  // per-datagram fragments when it does not
  auto pack_message = [&](QueuedOut queued, uint8_t channel) {
    bool reliable = queued.options.GetOr<ReliableKey>(true);
    bool ordered = queued.options.GetOr<OrderedKey>(true);
    uint8_t data_flags = 0;
    if (reliable) {
      data_flags |= kRecReliable;
    }
    if (ordered) {
      data_flags |= kRecOrdered;
    }
    // reliable and unreliable messages advance independent per-channel sequence
    // spaces so they can coexist on one channel.
    ChannelState& state = channels_[channel];
    SequenceId message_seq = reliable ? state.rel_send++ : state.unrel_send++;

    const size_t read_base = queued.payload->read_cursor();
    // the message starts at the read cursor: the send pipeline reserves
    // headroom in front of it
    const size_t total = queued.payload->readable_bytes();

    // small enough for one datagram, so it can share one with its neighbours.
    if (total <= unfrag_capacity) {
      size_t need = ZDTRecordSize(/*fragment=*/false, total);
      if (batch_bytes + need > mtu || batch.size() >= SentInfo::kMaxKeys) {
        flush_batch();
      }
      PendingRecord pending =
          MakeRecord(queued.payload, read_base, total, data_flags, channel,
                     message_seq, 0, 1, reliable);
      if (reliable) {
        // filed before the send so window accounting is right while the batch
        // fills; flush_batch() logs the packet_seq once it goes out.
        TrackReliable(pending, read_base, now, TransmissionLog{});
      }
      batch.push_back(std::move(pending));
      batch_bytes += need;
      return;
    }

    // too big for one datagram: each fragment fills its own and is its own
    // reliable unit.
    flush_batch();
    size_t fragment_count = (total + frag_capacity - 1) / frag_capacity;
    if (fragment_count > 255) {
      ZNET_LOG_ERROR(
          "ZDT: message of {} bytes needs {} fragments (>255), dropping.", total,
          fragment_count);
      return;
    }
    uint8_t frag_count = static_cast<uint8_t>(fragment_count);
    uint8_t frag_flags = static_cast<uint8_t>(data_flags | kRecFragment);
    for (uint8_t i = 0; i < frag_count; i++) {
      size_t rel_offset = static_cast<size_t>(i) * frag_capacity;
      size_t length = std::min(frag_capacity, total - rel_offset);
      PendingRecord pending =
          MakeRecord(queued.payload, read_base + rel_offset, length, frag_flags,
                     channel, message_seq, i, frag_count, reliable);
      WireSeq packet = SendBatch(0, &pending, 1);
      if (reliable) {
        TrackReliable(pending, read_base + rel_offset, now, MakeLog(packet));
      }
    }
  };

  // drain up front even with lanes non-empty: the ring is only the producers'
  // hand-off, the lanes are where this worker parks what the send window
  // refused. waiting for them to empty would back a shut window into the ring,
  // where overflow costs sends. bounded, so past this the ring fills and
  // Send() refuses, which a caller can at least see.
  if (staged_count_ < config_.outbound_queue_capacity) {
    StageOutbound();
  }

  while (true) {
    // and again as the lanes empty, so a message encoded while this loop runs
    // still goes out in this flush rather than waiting for the next
    if (staged_count_ == 0 && StageOutbound() == 0) {
      break;
    }
    // one message per lane per cycle: a lane the window refuses is skipped,
    // not waited on, and a bulk transfer takes one slot per pass, so it can
    // neither park another channel's traffic behind its backlog nor drain the
    // window dry before another lane gets a turn.
    bool sent_any = false;
    const size_t lanes = staged_.size();
    for (size_t step = 0; step < lanes; step++) {
      StagedLane& lane = staged_[(staged_cursor_ + step) % lanes];
      if (lane.messages.empty()) {
        continue;
      }
      const SendOptions& next = lane.messages.front().options;
      const bool next_reliable = next.GetOr<ReliableKey>(true);
      // only reliable traffic is windowed; unreliable sends are never held
      // back.
      if (next_reliable) {
        // both windows are global, but the lane is only skipped: another
        // lane's front may be unreliable and free to go
        if (unacked_.size() >= static_cast<size_t>(config_.max_messages_in_flight)) {
          continue;
        }
        // the real congestion window: anything past what an ack can describe
        // is resent for nothing and eventually trips max_retries.
        if (in_flight_datagrams_ >= static_cast<size_t>(SendWindow())) {
          continue;
        }
        if (!unacked_.empty()) {
          auto oldest = unacked_.lower_bound(MsgKey{lane.channel, true, 0, 0});
          if (oldest != unacked_.end() &&
              oldest->first.channel == lane.channel &&
              oldest->first.reliable &&
              channels_[lane.channel].rel_send - oldest->first.message_seq >=
                  kMaxSeqGap) {
            continue;  // stalled until its oldest unacked message is retired
          }
        }
      }
      QueuedOut queued = std::move(lane.messages.front());
      lane.messages.pop_front();
      staged_count_--;
      pack_message(std::move(queued), lane.channel);
      sent_any = true;
    }
    if (!sent_any) {
      break;  // every lane is empty or refused by the window
    }
    // rotate so a window with room for fewer messages than there are lanes
    // serves a different lane first next time
    staged_cursor_ = (staged_cursor_ + 1) % lanes;
  }
  flush_batch();  // whatever is left over goes out now, not next tick
}

size_t ZDTTransportLayer::StageOutbound() {
  size_t count = 0;
  QueuedOut queued;
  while (outbound_.Pop(queued)) {
    const uint8_t channel = queued.options.GetOr<ChannelKey>(0);
    StagedLane* lane = nullptr;
    for (StagedLane& candidate : staged_) {
      if (candidate.channel == channel) {
        lane = &candidate;
        break;
      }
    }
    if (lane == nullptr) {
      staged_.push_back(StagedLane{});
      staged_.back().channel = channel;
      lane = &staged_.back();
    }
    lane->messages.push_back(std::move(queued));
    count++;
  }
  staged_count_ += count;
  return count;
}

ZDTTransportLayer::PendingRecord ZDTTransportLayer::MakeRecord(
    const std::shared_ptr<Buffer>& owner, size_t offset, size_t length,
    uint8_t flags, uint8_t channel, SequenceId message_seq, uint8_t frag_index,
    uint8_t frag_count, bool reliable) {
  PendingRecord pending;
  pending.record.flags = flags;
  pending.record.channel = channel;
  pending.record.message_seq = static_cast<WireSeq>(message_seq);
  pending.record.frag_index = frag_index;
  pending.record.frag_count = frag_count;
  pending.record.length = static_cast<uint16_t>(length);
  pending.owner = owner;
  pending.payload = owner->data() + offset;
  pending.payload_len = length;
  pending.reliable = reliable;
  pending.key = MsgKey{channel, reliable, message_seq, frag_index};
  return pending;
}

void ZDTTransportLayer::TrackReliable(const PendingRecord& pending,
                                      size_t offset, TimePoint now,
                                      TransmissionLog log) {
  // rtt_.rto() can shrink, so this message may fall due before the cached deadline
  next_retransmit_scan_ = std::min(next_retransmit_scan_, now + rtt_.rto());
  // the probe measures silence after the newest send, so every send pushes it
  tail_probes_fired_ = 0;
  tail_probe_at_ = now + TailProbeDelay();
  unacked_[pending.key] = OutReliable{pending.owner,             // message
                                      offset,                    // offset
                                      pending.payload_len,       // length
                                      pending.record.channel,    // channel
                                      pending.record.message_seq,// message_seq
                                      pending.record.frag_index, // frag_index
                                      pending.record.frag_count, // frag_count
                                      pending.record.flags,      // data_flags
                                      now,                       // last_send
                                      1,                         // send_count
                                      log};              // packets
}

void ZDTTransportLayer::SendControl(uint8_t flags) {
  SendBatch(flags, nullptr, 0);  // header only, no records
}

WireSeq ZDTTransportLayer::SendBatch(uint8_t extra_flags,
                                     const PendingRecord* batch, size_t count) {
  if (!socket_ || !peer_) {
    return next_packet_seq_;
  }
  ZDTHeader header;
  header.flags = static_cast<uint8_t>(kFlagOnline | extra_flags);
  header.packet_seq = next_packet_seq_++;
  if (next_packet_seq_ == 0) {
    next_packet_seq_ = 1;  // skip the reserved sentinel on wraparound
  }
  // ack blocks are variable length, so the records get first claim on the MTU
  // and the encoder takes what is left. FlushOutbound packs against
  // kZDTHeaderReserve, so a batch it built always leaves room for some.
  size_t record_bytes = 0;
  for (size_t i = 0; i < count; i++) {
    record_bytes += ZDTRecordSize(batch[i].record.flags & kRecFragment,
                                  batch[i].payload_len);
  }
  const size_t mtu = connection_.mtu != 0
                         ? connection_.mtu
                         : ZDTPayloadForLinkMTU(config_.mtu_ladder.back(),
                                                peer_->ipv());
  const size_t used = kZDTHeaderSize + record_bytes;
  // how far back is worth describing: anything older the peer has already seen
  // acked, or it could not have kept sending. The +64 is slack for reordering.
  const size_t reportable = static_cast<size_t>(SendWindowCap()) + 64;
  ack_history_.Fill(header, mtu > used ? (mtu - used) / kZDTAckBlockSize : 0,
                    reportable);
#if ZNET_ENABLE_METRICS
  // counted here rather than inside Fill(), which stays const and free of the
  // metrics dependency
  for (uint8_t i = 0; i < header.block_count; i++) {
    metrics_.zdt.naks_sent += header.blocks[i].num_nack;
  }
#endif

  send_scratch_.Reset();
  Buffer& datagram = send_scratch_;
  WriteZDTHeader(datagram, header);
  SentInfo info;
  for (size_t i = 0; i < count; i++) {
    const PendingRecord& pending = batch[i];
    WriteZDTRecord(datagram, pending.record);
    if (pending.payload && pending.payload_len > 0) {
      datagram.Write(pending.payload, pending.payload_len);
    }
    if (pending.reliable) {
      // every reliable message here is retired when this packet is acked
      info.Add(pending.key);
    }
  }
  socket_->SendTo(*peer_, datagram.data(), datagram.size());
  ZNET_METRIC(metrics_.zdt.datagrams_sent++);
  ZNET_METRIC(metrics_.common.wire_bytes_sent += datagram.size());

  last_send_ = steady_clock::now();
  needs_ack_ = false;  // this datagram piggybacked our current ack
  info.send_time = last_send_;
  // replacing a live entry would leak its count, though a 16-bit sequence
  // wrapping onto one still tracked means far worse is already wrong
  RetireSentPacket(header.packet_seq);
  if (info.key_count > 0) {
    in_flight_datagrams_++;
  }
  sent_packets_[header.packet_seq] = info;
  return header.packet_seq;
}

void ZDTTransportLayer::RetireSentPacket(
    std::unordered_map<uint16_t, SentInfo>::iterator it) {
  if (it == sent_packets_.end()) {
    return;
  }
  if (it->second.key_count > 0 && in_flight_datagrams_ > 0) {
    in_flight_datagrams_--;
  }
  sent_packets_.erase(it);
}

void ZDTTransportLayer::RetireSentPacket(uint16_t packet_seq) {
  RetireSentPacket(sent_packets_.find(packet_seq));
}



// hard ceiling: the configured window, never past what the receiver's history
// can describe.
int ZDTTransportLayer::SendWindowCap() const {
  int cap = config_.max_datagrams_in_flight;
  if (cap > kZDTMaxDatagramsInFlight) {
    cap = kZDTMaxDatagramsInFlight;
  }
  return cap < 2 ? 2 : cap;
}

int ZDTTransportLayer::SendWindow() const {
  return congestion_.Window(SendWindowCap());
}

// the congestion signal is queueing delay, not loss. Reno-style halving on
// every drop settles at a window of ~1.2/sqrt(loss), six datagrams at 5%,
// which collapses throughput on a link that is lossy rather than congested.
// A full queue raises the round trip; a corrupted radio frame does not.
// Both congestion paths ask the same question, so they ask it in one place.
//
// A purely relative test is known to be wrong at the bottom of the range: at
// the few microseconds of a loopback or same-rack round trip, 1.25x is a margin
// under a microsecond, so the ordinary cost of keeping data in flight reads as
// congestion and the window sits at its floor on a path with no queue at all.
// Adding an absolute margin does open the window there, and is worth 18-65% at
// 1 KiB and above, but it costs ~30% at 64 B and makes that case bimodal, so it
// is not simply a better rule. See benchmarks/README.md.
void ZDTTransportLayer::OnNak(WireSeq packet_seq) {
  auto it = sent_packets_.find(packet_seq);
  if (it == sent_packets_.end()) {
    return;  // already retired, or too old to still be tracked
  }
  // an epoch last_send is the fast-retransmit marker: it puts the message past
  // any threshold, and tells the scan this was a reported loss rather than a
  // timeout. the scan is skipped until the soonest deadline, so it also has to
  // be pulled forward or the resend still waits out the RTO.
  for (const MsgKey& key : it->second) {
    auto msg = unacked_.find(key);
    if (msg != unacked_.end()) {
      msg->second.last_send = TimePoint{};
      next_retransmit_scan_ = TimePoint::min();
    }
  }
  // the peer reports this gap until filled, and it never will: the resend goes
  // out under a fresh packet_seq. drop it so one loss triggers one retransmit.
  RetireSentPacket(it);
  ZNET_METRIC(metrics_.zdt.naks_received++);
}

void ZDTTransportLayer::ProcessAcks(const ZDTHeader& header) {
  congestion_.OnAckArrived(header.ack);
  int newly_acked = 0;
  uint32_t offset = 0;
  for (uint8_t b = 0; b < header.block_count && offset < kZDTAckHistoryBits;
       b++) {
    for (uint8_t k = 0; k < header.blocks[b].num_ack; k++) {
      if (AckPacket(static_cast<WireSeq>(header.ack - offset))) {
        newly_acked++;
      }
      offset++;
    }
    for (uint8_t k = 0; k < header.blocks[b].num_nack; k++) {
      OnNak(static_cast<WireSeq>(header.ack - offset));
      offset++;
    }
  }
  if (newly_acked > 0) {
    congestion_.OnAcked(newly_acked, rtt_, next_packet_seq_, SendWindowCap());
    // progress breaks the silence; a duplicate ack does not, because a peer
    // that answers without retiring anything is describing exactly the stall
    // the probe exists to break
    tail_probes_fired_ = 0;
    tail_probe_at_ = unacked_.empty() ? TimePoint::max()
                                      : steady_clock::now() + TailProbeDelay();
  }
}

bool ZDTTransportLayer::AckPacket(WireSeq packet_seq) {
  if (packet_seq == 0) {
    return false;  // reserved sentinel: the peer had nothing to acknowledge yet
  }
  auto it = sent_packets_.find(packet_seq);
  if (it == sent_packets_.end()) {
    return false;
  }
  SentInfo info = it->second;
  RetireSentPacket(it);
  // packet_seq is unique per transmission, so there is no Karn ambiguity.
  const TimePoint now = steady_clock::now();
  rtt_.OnSample(now - info.send_time, now, config_.rto_min, config_.rto_max);
  // one datagram can carry several reliable messages; acking it retires all of
  // them. dropping each message's other transmissions keeps a sent_packets_
  // entry from outliving the unacked_ entry that owns it.
  for (const MsgKey& key : info) {
    auto msg = unacked_.find(key);
    if (msg == unacked_.end()) {
      continue;
    }
    for (WireSeq seq : msg->second.packets) {
      RetireSentPacket(seq);
    }
    unacked_.erase(msg);
  }
  return true;
}

void ZDTTransportLayer::RetransmitUnacked() {
  auto now = steady_clock::now();
  // walking every unacked message on every tick costs more than the retransmits
  // themselves: the window holds hundreds of entries and almost none are ever
  // due. Skip the scan until the soonest deadline actually arrives.
  if (now < next_retransmit_scan_) {
    return;
  }
  TimePoint earliest = TimePoint::max();
  bool timed_out = false;
  for (auto& entry : unacked_) {
    OutReliable& msg = entry.second;
    // OnNak already decided this one is lost; it is not evidence of a queue.
    const bool nak_forced = msg.last_send == TimePoint{};
    int backoff = std::min(msg.send_count - 1, 6);
    auto threshold = std::min(rtt_.rto() * (1 << backoff), config_.rto_max);
    if (now - msg.last_send < threshold) {
      earliest = std::min(earliest, msg.last_send + threshold);
      continue;
    }
    if (msg.send_count >= config_.max_retries) {
      ZNET_LOG_WARN("ZDT: reliable message to {} exceeded {} retries, closing.",
                    peer_->readable(), config_.max_retries);
      Close();
      return;
    }
    // one per datagram: batching would tie unrelated messages' recovery
    // together, and retransmits should be rare.
    PendingRecord pending =
        MakeRecord(msg.message, msg.offset, msg.length, msg.data_flags,
                   msg.channel, entry.first.message_seq, msg.frag_index,
                   msg.frag_count, /*reliable=*/true);
    WireSeq packet = SendBatch(0, &pending, 1);
    msg.packets.Add(packet);
    ZNET_METRIC(metrics_.zdt.retransmits++);
    msg.last_send = now;
    msg.send_count++;
    // a resend breaks the silence as well, so the tail probe restarts from it
    // (fired count kept: this is recovery traffic, not ack progress)
    tail_probe_at_ = now + TailProbeDelay();
    if (!nak_forced) {
      timed_out = true;
    }
    earliest = std::min(earliest, now + threshold);
  }
  if (timed_out) {
    congestion_.OnRetransmitTimeout(rtt_, next_packet_seq_);
  }
  next_retransmit_scan_ = earliest;
}

std::chrono::steady_clock::duration ZDTTransportLayer::TailProbeDelay() const {
  auto delay = std::chrono::duration_cast<steady_clock::duration>(
      std::chrono::duration<double, std::milli>(2.0 * rtt_.srtt_ms()));
  const auto floor =
      std::chrono::duration_cast<steady_clock::duration>(
          std::chrono::milliseconds(kZDTTailProbeFloorMs));
  if (delay < floor) {
    delay = floor;
  }
  delay *= std::int64_t{1} << (tail_probes_fired_ < 6 ? tail_probes_fired_ : 6);
  const auto ceiling =
      std::chrono::duration_cast<steady_clock::duration>(config_.rto_max);
  return delay > ceiling ? ceiling : delay;
}

void ZDTTransportLayer::MaybeTailProbe(TimePoint now) {
  if (now < tail_probe_at_ || unacked_.empty()) {
    return;
  }
  // the newest transmission is the likeliest tail loss. >= so that within one
  // burst, whose sends share a timestamp, the highest key (the actual tail)
  // wins.
  auto newest = unacked_.begin();
  for (auto it = unacked_.begin(); it != unacked_.end(); ++it) {
    if (it->second.last_send >= newest->second.last_send) {
      newest = it;
    }
  }
  OutReliable& msg = newest->second;
  PendingRecord pending =
      MakeRecord(msg.message, msg.offset, msg.length, msg.data_flags,
                 msg.channel, newest->first.message_seq, msg.frag_index,
                 msg.frag_count, /*reliable=*/true);
  WireSeq packet = SendBatch(0, &pending, 1);
  msg.packets.Add(packet);
  ZNET_METRIC(metrics_.zdt.tail_probes++);
  // last_send and send_count stay untouched: the RTO backstop keeps both its
  // schedule and its give-up accounting, the probe is extra traffic on top.
  tail_probes_fired_++;
  tail_probe_at_ = now + TailProbeDelay();
}

void ZDTTransportLayer::PruneSentPackets() {
  auto now = steady_clock::now();
  auto max_age = config_.rto_max * 4;
  for (auto it = sent_packets_.begin(); it != sent_packets_.end();) {
    if (now - it->second.send_time > max_age) {
      auto stale = it++;
      RetireSentPacket(stale);
    } else {
      ++it;
    }
  }
}

SequenceId ZDTTransportLayer::ReconstructSeqFor(const ZDTRecord& record) {
  ChannelState& channel = channels_[record.channel];
  if (record.flags & kRecReliable) {
    return ReconstructSeq(record.message_seq, channel.rel_expected);
  }
  SequenceId expected =
      channel.unrel_started ? channel.unrel_last + 1 : SequenceId{0};
  return ReconstructSeq(record.message_seq, expected);
}

void ZDTTransportLayer::DeliverMessage(const ZDTRecord& record,
                                       std::shared_ptr<Buffer> payload) {
  const bool reliable = record.flags & kRecReliable;
  const bool ordered = record.flags & kRecOrdered;
  const SequenceId seq = ReconstructSeqFor(record);

  // unreliable + unordered: no state, and nothing retransmits, so no dedup.
  if (!reliable && !ordered) {
    ready_.push_back(std::move(payload));
    return;
  }

  ChannelState& channel = channels_[record.channel];

  // unreliable + ordered (sequenced): deliver only strictly-newer messages,
  // drop anything older-or-equal (late/duplicate). Uses the unreliable substream.
  if (!reliable && ordered) {
    if (!channel.unrel_started || seq > channel.unrel_last) {
      channel.unrel_started = true;
      channel.unrel_last = seq;
      ready_.push_back(std::move(payload));
    } else {
      ZNET_METRIC(metrics_.zdt.duplicates_dropped++);
    }
    return;
  }

  // reliable + unordered: deliver on arrival but dedup retransmits.
  // `rel_expected` is a low watermark (everything below delivered);
  // `rel_delivered_ahead` holds delivered seqs at/above it that arrived early.
  if (reliable && !ordered) {
    if (seq < channel.rel_expected ||
        channel.rel_delivered_ahead.count(seq) != 0) {
      ZNET_METRIC(metrics_.zdt.duplicates_dropped++);
      return;  // duplicate
    }
    ready_.push_back(std::move(payload));
    channel.rel_delivered_ahead.insert(seq);
    while (channel.rel_delivered_ahead.count(channel.rel_expected) != 0) {
      channel.rel_delivered_ahead.erase(channel.rel_expected);
      channel.rel_expected++;
    }
    return;
  }

  // reliable + ordered: buffer out-of-order, deliver the contiguous run.
  if (seq < channel.rel_expected) {
    ZNET_METRIC(metrics_.zdt.duplicates_dropped++);
    return;  // already delivered (duplicate / retransmit)
  }
  if (seq == channel.rel_expected) {
    ready_.push_back(std::move(payload));
    channel.rel_expected++;
    auto it = channel.rel_reorder.find(channel.rel_expected);
    while (it != channel.rel_reorder.end()) {
      ready_.push_back(std::move(it->second));
      channel.rel_reorder.erase(it);
      channel.rel_expected++;
      it = channel.rel_reorder.find(channel.rel_expected);
    }
  } else {
    channel.rel_reorder.emplace(seq, std::move(payload));  // future
  }
}

void ZDTTransportLayer::CheckTimers() {
  auto now = steady_clock::now();
  if (idle_timeout_.count() > 0 && now - last_recv_ > idle_timeout_) {
    ZNET_LOG_DEBUG("ZDT: closing {} due to idle timeout", peer_->readable());
    Close();
    return;
  }
  if (keepalive_interval_.count() > 0 &&
      now - last_send_ > keepalive_interval_) {
    SendControl(kFlagPing);
  }
}

void ZDTTransportLayer::Update() {
  ZNET_ZDT_ENTER_DOMAIN(worker_domain_);
  if (is_closed_) {
    return;
  }
  if (drains_own_socket_) {
    DrainSocket();
  }
  ProcessInbound();
  if (is_closed_) {
    return;
  }
  RetransmitUnacked();
  if (is_closed_) {
    return;
  }
  MaybeTailProbe(steady_clock::now());
  CheckTimers();
  if (is_closed_) {
    return;
  }
  Flush();
  PruneSentPackets();
  PruneReassembly();
}

void ZDTTransportLayer::Flush() {
  ZNET_ZDT_ENTER_DOMAIN(worker_domain_);
  if (is_closed_) {
    return;
  }
  FlushOutbound();
  // if we still owe an ack and no outgoing datagram carried it, send a
  // standalone one.
  if (needs_ack_) {
    SendControl(0);
  }
}

Result ZDTTransportLayer::Close(CloseOptions options) {
  (void)options;
  if (is_closed_.exchange(true)) {
    return Result::AlreadyDisconnected;
  }
  // Close() runs on the application's thread while every protocol field belongs
  // to the session worker, so the FIN is written by hand rather than through
  // SendBatch: no packet_seq, no sent_packets_ record, no ack. packet_seq 0 is
  // the reserved "nothing to acknowledge" sentinel the peer already ignores,
  // and concurrent sendto() on one socket is safe.
  if (socket_ && peer_) {
    ZDTHeader header;
    header.flags = static_cast<uint8_t>(kFlagOnline | kFlagFin);
    header.packet_seq = 0;
    Buffer datagram(Endianness::BigEndian);
    WriteZDTHeader(datagram, header);
    socket_->SendTo(*peer_, datagram.data(), datagram.size());
  }
  if (drains_own_socket_ && socket_) {
    // shut down rather than close: the session's worker may be inside
    // RecvFrom() on this socket via DrainSocket(). See UDPSocket::Shutdown().
    socket_->Shutdown();
  }
  return Result::Success;
}

bool ZDTTransportLayer::OnDataFragment(const ZDTRecord& record,
                                       const uint8_t* data, size_t len) {
  if (record.frag_count == 0 || record.frag_index >= record.frag_count) {
    return true;  // malformed, but nothing to retransmit that would help
  }
  bool reliable = record.flags & kRecReliable;
  MsgKey key{record.channel, reliable, ReconstructSeqFor(record), 0};
  auto existing = reassembly_.find(key);
  if (existing == reassembly_.end() &&
      (reassembly_.size() >= config_.max_reassemblies ||
       reassembly_bytes_ >= config_.max_reassembly_bytes)) {
    // at capacity, and this fragment would start a new message. refusing to
    // acknowledge it makes the sender try again once we have room; accepting
    // and then dropping it would lose the message, because the sender retires a
    // fragment the moment it is acked. Fragments for messages already in
    // progress are always taken, so the backlog can still drain.
    ZNET_METRIC(metrics_.zdt.reassemblies_dropped++);
    return !reliable;  // unreliable senders never retransmit, so let it go
  }
  Reassembly& assembly = reassembly_[key];
  if (assembly.frag_count == 0) {
    assembly.frag_count = record.frag_count;
    assembly.first_seen = steady_clock::now();
  }
  if (assembly.frag_count != record.frag_count) {
    return true;  // inconsistent fragment count for this message
  }
  std::vector<uint8_t>& slot = assembly.fragments[record.frag_index];
  reassembly_bytes_ -= slot.size();  // a retransmit overwrites its own copy
  slot.assign(data, data + len);
  reassembly_bytes_ += slot.size();
  if (assembly.fragments.size() != assembly.frag_count) {
    return true;  // still incomplete
  }
  auto full = std::make_shared<Buffer>();
  for (auto& fragment : assembly.fragments) {  // std::map -> ascending index
    if (!fragment.second.empty()) {
      full->Write(fragment.second.data(), fragment.second.size());
    }
    reassembly_bytes_ -= fragment.second.size();
  }
  reassembly_.erase(key);
  // hand the reassembled whole up as a plain message, not a fragment
  ZDTRecord whole = record;
  whole.flags = static_cast<uint8_t>(record.flags & ~kRecFragment);
  whole.frag_index = 0;
  whole.frag_count = 1;
  DeliverMessage(whole, std::move(full));
  return true;
}

void ZDTTransportLayer::PruneReassembly() {
  auto now = steady_clock::now();
  for (auto it = reassembly_.begin(); it != reassembly_.end();) {
    if (now - it->second.first_seen > config_.reassembly_timeout) {
      for (const auto& fragment : it->second.fragments) {
        reassembly_bytes_ -= fragment.second.size();
      }
      ZNET_METRIC(metrics_.zdt.reassemblies_dropped++);
      it = reassembly_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace backends
}  // namespace znet
