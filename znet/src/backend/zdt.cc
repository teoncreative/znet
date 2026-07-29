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

#include "znet/backends/zdt.h"

#include "znet/error.h"
#include "znet/logger.h"
#include "znet/util.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace znet {
namespace backends {

using steady_clock = std::chrono::steady_clock;

namespace {
// 16-bit sequence comparison that tolerates wraparound.
inline bool SeqGreater(uint16_t a, uint16_t b) {
  return static_cast<int16_t>(a - b) > 0;
}
inline bool SeqLess(uint16_t a, uint16_t b) {
  return static_cast<int16_t>(a - b) < 0;
}
}  // namespace

ZDTTransportLayer::TransmissionLog ZDTTransportLayer::MakeLog(WireSeq seq) {
  TransmissionLog log;
  log.Add(seq);
  return log;
}

// ---------------------------------------------------------------------------
// Wire header
// ---------------------------------------------------------------------------

void WriteZDTHeader(Buffer& buffer, const ZDTHeader& header) {
  buffer.WriteInt<uint8_t>(header.flags);
  buffer.WriteInt<uint16_t>(header.packet_seq);
  buffer.WriteInt<uint16_t>(header.ack);
  buffer.WriteInt<uint8_t>(header.block_count);
  for (uint8_t i = 0; i < header.block_count; i++) {
    buffer.WriteInt<uint8_t>(header.blocks[i].num_ack);
    buffer.WriteInt<uint8_t>(header.blocks[i].num_nack);
  }
}

bool ReadZDTHeader(Buffer& buffer, ZDTHeader& out_header) {
  if (buffer.readable_bytes() < kZDTHeaderSize) {
    return false;
  }
  out_header.flags = buffer.ReadInt<uint8_t>();
  if (!(out_header.flags & kFlagOnline)) {
    return false;  // offline (handshake) message, not an online datagram
  }
  out_header.packet_seq = buffer.ReadInt<uint16_t>();
  out_header.ack = buffer.ReadInt<uint16_t>();
  uint8_t count = buffer.ReadInt<uint8_t>();
  // a peer cannot make us read past the datagram, nor overrun the array
  if (count > kZDTMaxAckBlocks ||
      buffer.readable_bytes() < count * kZDTAckBlockSize) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    out_header.blocks[i].num_ack = buffer.ReadInt<uint8_t>();
    out_header.blocks[i].num_nack = buffer.ReadInt<uint8_t>();
  }
  out_header.block_count = count;
  return true;
}

void WriteZDTRecord(Buffer& buffer, const ZDTRecord& record) {
  buffer.WriteInt<uint8_t>(record.flags);
  buffer.WriteInt<uint8_t>(record.channel);
  buffer.WriteInt<uint16_t>(record.message_seq);
  if (record.flags & kRecFragment) {
    buffer.WriteInt<uint8_t>(record.frag_index);
    buffer.WriteInt<uint8_t>(record.frag_count);
  }
  buffer.WriteInt<uint16_t>(record.length);
}

bool ReadZDTRecord(Buffer& buffer, ZDTRecord& out_record) {
  if (buffer.readable_bytes() < kZDTRecordHeaderSize) {
    return false;
  }
  out_record.flags = buffer.ReadInt<uint8_t>();
  out_record.channel = buffer.ReadInt<uint8_t>();
  out_record.message_seq = buffer.ReadInt<uint16_t>();
  if (out_record.flags & kRecFragment) {
    if (buffer.readable_bytes() < 4) {  // frag pair plus length
      return false;
    }
    out_record.frag_index = buffer.ReadInt<uint8_t>();
    out_record.frag_count = buffer.ReadInt<uint8_t>();
  } else {
    out_record.frag_index = 0;
    out_record.frag_count = 1;
  }
  out_record.length = buffer.ReadInt<uint16_t>();
  // a truncated or lying length must not walk the reader off the datagram
  return buffer.readable_bytes() >= out_record.length;
}

void WriteOfflineHeader(Buffer& buffer, ZDTOfflineMsg id) {
  buffer.WriteInt<uint8_t>(static_cast<uint8_t>(id));
  buffer.Write(kZDTMagic.data(), kZDTMagic.size());
}

bool ReadOfflineHeader(Buffer& buffer, ZDTOfflineMsg& out_id) {
  if (buffer.readable_bytes() < 1 + kZDTMagic.size()) {
    return false;
  }
  uint8_t id = buffer.ReadInt<uint8_t>();
  if (id & kFlagOnline) {
    return false;  // online datagram, not an offline message
  }
  std::array<uint8_t, kZDTMagic.size()> magic{};
  buffer.Read(magic.data(), magic.size());
  if (magic != kZDTMagic) {
    return false;
  }
  out_id = static_cast<ZDTOfflineMsg>(id);
  return true;
}

// ---------------------------------------------------------------------------
// Return-routability cookie
// ---------------------------------------------------------------------------

ZDTCookie ComputeCookie(const uint8_t* secret, size_t secret_len,
                        const std::string& peer_readable, uint32_t epoch) {
  std::string message = peer_readable;
  message.push_back('|');
  for (int i = 0; i < 4; i++) {
    message.push_back(static_cast<char>((epoch >> (i * 8)) & 0xFFu));
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  HMAC(EVP_sha256(), secret, static_cast<int>(secret_len),
       reinterpret_cast<const unsigned char*>(message.data()), message.size(),
       digest, &digest_len);
  ZDTCookie cookie{};
  size_t copy = std::min<size_t>(cookie.size(), digest_len);
  std::memcpy(cookie.data(), digest, copy);
  return cookie;
}

bool ConstTimeEqual(const ZDTCookie& a, const ZDTCookie& b) {
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

uint64_t GenerateGuid() {
  uint64_t guid = 0;
  RAND_bytes(reinterpret_cast<unsigned char*>(&guid), sizeof(guid));
  return guid;
}

// ---------------------------------------------------------------------------
// UDPSocket
// ---------------------------------------------------------------------------

UDPSocket::~UDPSocket() {
  Close();
}

Result UDPSocket::Open(InetProtocolVersion ipv) {
  socket_ = socket(GetDomainByInetProtocolVersion(ipv), SOCK_DGRAM, 0);
  if (!IsValidSocketHandle(socket_)) {
    ZNET_LOG_ERROR("ZDT: failed to create UDP socket: {}", GetLastErrorInfo());
    return Result::CannotCreateSocket;
  }
  const char option = 1;
  setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
  return Result::Success;
}

Result UDPSocket::Bind(const InetAddress& addr) {
  if (bind(socket_, addr.handle_ptr(), addr.addr_size()) != 0) {
    ZNET_LOG_ERROR("ZDT: failed to bind UDP socket to {}: {}", addr.readable(),
                   GetLastErrorInfo());
    return Result::CannotBind;
  }
  return Result::Success;
}

bool UDPSocket::SendTo(const InetAddress& addr, const void* data, size_t len) {
#ifdef TARGET_WIN
  int n = sendto(socket_, static_cast<const char*>(data), static_cast<int>(len),
                 0, addr.handle_ptr(), addr.addr_size());
#else
  ssize_t n = sendto(socket_, static_cast<const char*>(data), len, 0,
                     addr.handle_ptr(), addr.addr_size());
#endif
  if (n < 0) {
    ZNET_LOG_DEBUG("ZDT: sendto {} failed: {}", addr.readable(),
                   GetLastErrorInfo());
    return false;
  }
  return static_cast<size_t>(n) == len;
}

RecvResult UDPSocket::RecvFrom(void* data, size_t cap, size_t& out_len,
                               std::shared_ptr<InetAddress>& out_from) {
  sockaddr_storage from{};
  socklen_t from_len = sizeof(from);
#ifdef TARGET_WIN
  int n = recvfrom(socket_, static_cast<char*>(data), static_cast<int>(cap), 0,
                   reinterpret_cast<sockaddr*>(&from), &from_len);
#else
  ssize_t n = recvfrom(socket_, static_cast<char*>(data), cap, 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
  if (n < 0) {
#ifdef TARGET_WIN
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
      return RecvResult::WouldBlock;
    }
#else
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return RecvResult::WouldBlock;
    }
#endif
    return RecvResult::Error;
  }
  out_len = static_cast<size_t>(n);
  out_from = std::shared_ptr<InetAddress>(
      InetAddress::from(reinterpret_cast<sockaddr*>(&from)));
  return RecvResult::Received;
}

bool UDPSocket::SetBlocking(bool blocking) {
  return SetSocketBlocking(socket_, blocking);
}

bool UDPSocket::SetReceiveTimeout(std::chrono::milliseconds timeout) {
#ifdef TARGET_WIN
  DWORD ms = static_cast<DWORD>(timeout.count());
  return setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  return setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool UDPSocket::SetReceiveBufferSize(int bytes) {
  return setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                    reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool UDPSocket::SetDontFragment(bool enabled) {
#if defined(TARGET_LINUX)
  int value = enabled ? IP_PMTUDISC_DO : IP_PMTUDISC_WANT;
  return setsockopt(socket_, IPPROTO_IP, IP_MTU_DISCOVER, &value,
                    sizeof(value)) == 0;
#elif defined(TARGET_APPLE)
  int value = enabled ? 1 : 0;
  return setsockopt(socket_, IPPROTO_IP, IP_DONTFRAG, &value, sizeof(value)) == 0;
#elif defined(TARGET_WIN)
  DWORD value = enabled ? 1 : 0;
  return setsockopt(socket_, IPPROTO_IP, IP_DONTFRAGMENT,
                    reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
  (void)enabled;
  return false;
#endif
}

std::shared_ptr<InetAddress> UDPSocket::local_address() {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (getsockname(socket_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    ZNET_LOG_ERROR("ZDT: getsockname failed: {}", GetLastErrorInfo());
    return nullptr;
  }
  return std::shared_ptr<InetAddress>(
      InetAddress::from(reinterpret_cast<sockaddr*>(&ss)));
}

bool UDPSocket::Shutdown() {
  return ShutdownSocket(socket_);
}

Result UDPSocket::Close() {
  if (IsValidSocketHandle(socket_)) {
    CloseSocket(socket_);
    socket_ = kSocketInvalid;
  }
  return Result::Success;
}

// ---------------------------------------------------------------------------
// ZDTInbox
// ---------------------------------------------------------------------------

bool ZDTInbox::Push(const uint8_t* data, size_t len, size_t limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= limit) {
    dropped_++;
    return false;
  }
  queue_.emplace_back(data, data + len);
  return true;
}

void ZDTInbox::Drain(std::deque<std::vector<uint8_t>>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out.swap(queue_);
}

size_t ZDTInbox::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

// ---------------------------------------------------------------------------
// ZDTTransportLayer
// ---------------------------------------------------------------------------

ZDTTransportLayer::ZDTTransportLayer(std::shared_ptr<UDPSocket> socket,
                                     std::shared_ptr<InetAddress> peer,
                                     ZDTOptions config, bool drains_own_socket,
                                     std::shared_ptr<ZDTInbox> inbox,
                                     ZDTConnection connection)
    : socket_(std::move(socket)),
      peer_(std::move(peer)),
      config_(std::move(config)),
      drains_own_socket_(drains_own_socket),
      inbox_(inbox ? std::move(inbox) : std::make_shared<ZDTInbox>()),
      connection_(connection),
      last_recv_(steady_clock::now()),
      last_send_(steady_clock::now()) {
  rto_ = compat::Clamp(std::chrono::milliseconds(200), config_.rto_min,
                    config_.rto_max);
}

ZDTTransportLayer::~ZDTTransportLayer() {
  Close();
}

std::shared_ptr<Buffer> ZDTTransportLayer::Receive() {
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
  std::unique_lock<std::mutex> lock(outbound_mutex_);
  // outbound_count_, not outbound_.size(): FlushOutbound() moves the whole
  // queue into a local while it works, so size() reads zero mid-flush and the
  // bound would not hold.
  if (outbound_count_ >= config_.max_outbound_messages) {
    // refusing is the backpressure signal to the caller.
    ZNET_LOG_WARN("ZDT: outbound queue full ({}), dropping packet!",
                  outbound_count_);
    return false;
  }
  outbound_.push_back(QueuedOut{std::move(buffer), options});
  const bool was_idle = outbound_count_ == 0 && staged_.empty();
  outbound_count_++;
  lock.unlock();
  // only on the idle edge: with anything already queued a flush is on its way
  // and the notify would be pure overhead on the hot path.
  if (was_idle) {
    WakeWorker();
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
  out.zdt.srtt_us = static_cast<uint32_t>(srtt_ms_ * 1000.0);
  out.zdt.rto_us = static_cast<uint32_t>(rto_.count() * 1000);
  out.zdt.in_flight = static_cast<uint32_t>(unacked_.size());
  out.zdt.mtu = connection_.mtu;
  {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    out.common.outbound_queued = static_cast<uint32_t>(outbound_count_);
  }
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
      RecordRemoteSeq(header.packet_seq);
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
  uint16_t mtu =
      connection_.mtu != 0 ? connection_.mtu : config_.mtu_ladder.back();
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
  std::vector<PendingRecord> batch;
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

  // one reading for the whole flush. reading the clock per message showed up as
  // the single largest cost once the retransmit scan was fixed, and a batch
  // takes microseconds, so a shared timestamp is accurate enough for the RTO
  // deadlines it feeds.
  const TimePoint now = steady_clock::now();

  size_t consumed = 0;

  while (true) {
    if (staged_.empty()) {
      std::lock_guard<std::mutex> lock(outbound_mutex_);
      if (outbound_.empty()) {
        break;
      }
      staged_.swap(outbound_);  // O(1); staged_ is empty here
    }
    {
      const SendOptions& next = staged_.front().options;
      const bool next_reliable = next.GetOr<ReliableKey>(true);
      // only reliable traffic is windowed; unreliable sends are never held back.
      if (next_reliable &&
          unacked_.size() >= static_cast<size_t>(config_.cwnd)) {
        break;
      }
      // the real congestion window: anything past what an ack can describe is
      // resent for nothing and eventually trips max_retries.
      if (next_reliable &&
          sent_packets_.size() >= static_cast<size_t>(SendWindow())) {
        break;
      }
      if (next_reliable && !unacked_.empty()) {
        uint8_t next_channel = next.GetOr<ChannelKey>(0);
        auto oldest = unacked_.lower_bound(MsgKey{next_channel, true, 0, 0});
        if (oldest != unacked_.end() && oldest->first.channel == next_channel &&
            oldest->first.reliable &&
            channels_[next_channel].rel_send - oldest->first.message_seq >=
                kMaxSeqGap) {
          break;  // stalled until the oldest unacked message is retired
        }
      }
    }
    QueuedOut queued = std::move(staged_.front());
    staged_.pop_front();
    consumed++;

    uint8_t channel = queued.options.GetOr<ChannelKey>(0);
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
    const size_t total = queued.payload->size();

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
      continue;
    }

    // too big for one datagram: each fragment fills its own and is its own
    // reliable unit.
    flush_batch();
    size_t fragment_count = (total + frag_capacity - 1) / frag_capacity;
    if (fragment_count > 255) {
      ZNET_LOG_ERROR(
          "ZDT: message of {} bytes needs {} fragments (>255), dropping.", total,
          fragment_count);
      continue;
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
  }
  flush_batch();  // whatever is left over goes out now, not next tick

  if (consumed != 0) {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    outbound_count_ -= consumed;
  }
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
  // rto_ can shrink, so this message may fall due before the cached deadline
  next_retransmit_scan_ = std::min(next_retransmit_scan_, now + rto_);
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
  const size_t mtu =
      connection_.mtu != 0 ? connection_.mtu : config_.mtu_ladder.back();
  const size_t used = kZDTHeaderSize + record_bytes;
  FillAck(header, mtu > used ? (mtu - used) / kZDTAckBlockSize : 0);

  Buffer datagram(Endianness::BigEndian);
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
  sent_packets_[header.packet_seq] = info;
  return header.packet_seq;
}

namespace {

// bit b of the history means "packet_seq (remote_ack_seq_ - b) arrived".
bool HistoryBit(const std::array<uint64_t, kZDTAckHistoryWords>& bits, size_t b) {
  return (bits[b / 64] >> (b % 64)) & 1u;
}

void SetHistoryBit(std::array<uint64_t, kZDTAckHistoryWords>& bits, size_t b) {
  bits[b / 64] |= (uint64_t{1} << (b % 64));
}

// remote_ack_seq_ advanced by `n`, so every recorded packet is now `n` further
// back: bit b becomes bit b+n, and the newly exposed low bits are unknown.
void ShiftHistoryUp(std::array<uint64_t, kZDTAckHistoryWords>& bits, size_t n) {
  if (n >= kZDTAckHistoryBits) {
    bits.fill(0);
    return;
  }
  const size_t words = n / 64;
  const size_t rem = n % 64;
  for (size_t i = kZDTAckHistoryWords; i-- > 0;) {
    uint64_t v = 0;
    if (i >= words) {
      v = bits[i - words] << rem;
      if (rem != 0 && i > words) {
        v |= bits[i - words - 1] >> (64 - rem);
      }
    }
    bits[i] = v;
  }
}

}  // namespace

// walks the received-history backwards from `ack` and run-length encodes it.
// the first block always starts on a received run, because bit 0 is `ack`
// itself. Trailing blocks that describe only missing packets are dropped: the
// sender learns nothing from "everything older than this is still missing",
// and a later acknowledgement will carry them once something in between lands.
void ZDTTransportLayer::FillAck(ZDTHeader& header, size_t max_blocks) {
  header.ack = remote_ack_seq_;
  header.block_count = 0;
  if (!has_remote_seq_ || max_blocks == 0) {
    return;
  }
  if (max_blocks > kZDTMaxAckBlocks) {
    max_blocks = kZDTMaxAckBlocks;
  }
  // bounded by what the peer has actually sent; zeroes past that are history
  // never observed, and reporting them would invent losses on a new connection.
  // also bounded by the window, since anything older than that the peer has
  // already seen acked or it could not have kept sending. truncating is safe
  // where hiding a gap is not: runs are positional, so folding one into an ack
  // run would falsely retire the message.
  size_t limit = history_valid_bits_;
  const size_t reportable = static_cast<size_t>(SendWindowCap()) + 64;
  if (limit > reportable) {
    limit = reportable;
  }
  size_t b = 0;
  while (b < limit && header.block_count < max_blocks) {
    size_t acks = 0;
    while (b < limit && acks < 255 && HistoryBit(remote_ack_bits_, b)) {
      acks++;
      b++;
    }
    size_t nacks = 0;
    while (b < limit && nacks < 255 && !HistoryBit(remote_ack_bits_, b)) {
      nacks++;
      b++;
    }
    if (acks == 0 && nacks == 0) {
      break;
    }
    header.blocks[header.block_count].num_ack = static_cast<uint8_t>(acks);
    header.blocks[header.block_count].num_nack = static_cast<uint8_t>(nacks);
    header.block_count++;
  }
  while (header.block_count > 0 &&
         header.blocks[header.block_count - 1].num_ack == 0) {
    header.block_count--;
  }
#if ZNET_ENABLE_METRICS
  for (uint8_t i = 0; i < header.block_count; i++) {
    metrics_.zdt.naks_sent += header.blocks[i].num_nack;
  }
#endif
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
  const int cap = SendWindowCap();
  int w = static_cast<int>(cwnd_);
  if (w < 2) {
    w = 2;  // always allow enough in flight to make forward progress
  }
  return w > cap ? cap : w;
}

// the congestion signal is queueing delay, not loss. Reno-style halving on
// every drop settles at a window of ~1.2/sqrt(loss), six datagrams at 5%,
// which collapses throughput on a link that is lossy rather than congested.
// A full queue raises the round trip; a corrupted radio frame does not.
void ZDTTransportLayer::OnCongestionAck(int acked_datagrams) {
  if (acked_datagrams <= 0) {
    return;
  }
  if (has_rtt_ && rtt_min_ms_ > 0.0 &&
      srtt_ms_ > rtt_min_ms_ * kZDTQueueingRttRatio && cwnd_ > 4.0) {
    if (!in_loss_recovery_) {
      in_loss_recovery_ = true;
      loss_recovery_until_ = next_packet_seq_;
      cwnd_ *= kZDTQueueingBackoff;
      ssthresh_ = cwnd_;
    }
    return;
  }
  if (cwnd_ < ssthresh_) {
    cwnd_ += static_cast<double>(acked_datagrams);
  } else {
    cwnd_ += static_cast<double>(acked_datagrams) / cwnd_;
  }
  const double cap = static_cast<double>(SendWindowCap());
  if (cwnd_ > cap) {
    cwnd_ = cap;
  }
}

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
  sent_packets_.erase(it);
  ZNET_METRIC(metrics_.zdt.naks_received++);
}

void ZDTTransportLayer::RecordRemoteSeq(WireSeq packet_seq) {
  if (!has_remote_seq_) {
    has_remote_seq_ = true;
    remote_ack_seq_ = packet_seq;
    remote_ack_bits_.fill(0);
    SetHistoryBit(remote_ack_bits_, 0);  // bit 0 is remote_ack_seq_ itself
    history_valid_bits_ = 1;
    return;
  }
  if (SeqGreater(packet_seq, remote_ack_seq_)) {
    const uint16_t shift = static_cast<uint16_t>(packet_seq - remote_ack_seq_);
    ShiftHistoryUp(remote_ack_bits_, shift);
    remote_ack_seq_ = packet_seq;
    SetHistoryBit(remote_ack_bits_, 0);
    history_valid_bits_ += shift;
    if (history_valid_bits_ > kZDTAckHistoryBits) {
      history_valid_bits_ = kZDTAckHistoryBits;
    }
  } else if (SeqLess(packet_seq, remote_ack_seq_)) {
    const uint16_t back = static_cast<uint16_t>(remote_ack_seq_ - packet_seq);
    if (back < kZDTAckHistoryBits) {
      SetHistoryBit(remote_ack_bits_, back);  // a late arrival, not a gap
    }
  }
  // equal -> duplicate of the current highest; nothing to do
}

void ZDTTransportLayer::ProcessAcks(const ZDTHeader& header) {
  // leaving recovery on the first ack of something sent after the loss is what
  // keeps one burst from halving the window more than once
  if (in_loss_recovery_ && !SeqLess(header.ack, loss_recovery_until_)) {
    in_loss_recovery_ = false;
  }
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
    OnCongestionAck(newly_acked);
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
  sent_packets_.erase(it);
  // packet_seq is unique per transmission, so there is no Karn ambiguity.
  const TimePoint now = steady_clock::now();
  UpdateRtt(now - info.send_time, now);
  // one datagram can carry several reliable messages; acking it retires all of
  // them. dropping each message's other transmissions keeps a sent_packets_
  // entry from outliving the unacked_ entry that owns it.
  for (const MsgKey& key : info) {
    auto msg = unacked_.find(key);
    if (msg == unacked_.end()) {
      continue;
    }
    for (WireSeq seq : msg->second.packets) {
      sent_packets_.erase(seq);
    }
    unacked_.erase(msg);
  }
  return true;
}

void ZDTTransportLayer::UpdateRtt(std::chrono::steady_clock::duration sample,
                                  TimePoint now) {
  double ms = std::chrono::duration<double, std::milli>(sample).count();
  if (ms < 0.0) {
    return;
  }
  if (!has_rtt_) {
    has_rtt_ = true;
    srtt_ms_ = ms;
    rttvar_ms_ = ms / 2.0;
    rtt_min_ms_ = ms;
    rtt_min_stamp_ = now;
  } else {
    // a windowed minimum, not a lifetime one. the whole controller is measured
    // against this, so a stale floor left by a route change would read the new
    // baseline as permanent queueing and pin the window at its lower bound.
    const bool stale = now - rtt_min_stamp_ >
                       std::chrono::milliseconds(kZDTRttMinWindowMs);
    if (ms < rtt_min_ms_ || rtt_min_ms_ <= 0.0 || stale) {
      rtt_min_ms_ = ms;
      rtt_min_stamp_ = now;
    }
    rttvar_ms_ = 0.75 * rttvar_ms_ + 0.25 * std::fabs(srtt_ms_ - ms);
    srtt_ms_ = 0.875 * srtt_ms_ + 0.125 * ms;
  }
  auto rto = std::chrono::milliseconds(
      static_cast<long long>(srtt_ms_ + 4.0 * rttvar_ms_));
  rto_ = compat::Clamp(rto, config_.rto_min, config_.rto_max);
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
    auto threshold = std::min(rto_ * (1 << backoff), config_.rto_max);
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
    if (!nak_forced) {
      timed_out = true;
    }
    earliest = std::min(earliest, now + threshold);
  }
  // a timeout on a lossy path is usually still just loss, so it only collapses
  // the window when the round trip says a queue is involved
  if (timed_out) {
    const bool queueing = has_rtt_ && rtt_min_ms_ > 0.0 &&
                          srtt_ms_ > rtt_min_ms_ * kZDTQueueingRttRatio;
    if (queueing) {
      // guarded like the delay path: a burst of timeouts is one event, and
      // collapsing once per message in it would take several round trips of
      // slow start to undo.
      if (!in_loss_recovery_) {
        ssthresh_ = cwnd_ / 2.0;
        if (ssthresh_ < 2.0) {
          ssthresh_ = 2.0;
        }
        cwnd_ = 2.0;
        in_loss_recovery_ = true;
        loss_recovery_until_ = next_packet_seq_;
      }
    } else {
      cwnd_ *= 0.9;
      if (cwnd_ < 8.0) {
        cwnd_ = 8.0;
      }
    }
  }
  next_retransmit_scan_ = earliest;
}

void ZDTTransportLayer::PruneSentPackets() {
  auto now = steady_clock::now();
  auto max_age = config_.rto_max * 4;
  for (auto it = sent_packets_.begin(); it != sent_packets_.end();) {
    if (now - it->second.send_time > max_age) {
      it = sent_packets_.erase(it);
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
  if (now - last_recv_ > config_.idle_timeout) {
    ZNET_LOG_DEBUG("ZDT: closing {} due to idle timeout", peer_->readable());
    Close();
    return;
  }
  if (now - last_send_ > config_.keepalive_interval) {
    SendControl(kFlagPing);
  }
}

void ZDTTransportLayer::Update() {
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
  CheckTimers();
  if (is_closed_) {
    return;
  }
  Flush();
  PruneSentPackets();
  PruneReassembly();
}

void ZDTTransportLayer::Flush() {
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
  // Close() is public API and is normally called from the application's thread,
  // while every other protocol field belongs to the session worker. So the FIN
  // is written by hand here instead of going through SendBatch: it takes no
  // packet_seq, records nothing in sent_packets_ and carries no ack. packet_seq
  // 0 is the reserved "nothing to acknowledge" sentinel, which the peer already
  // ignores. Concurrent sendto() on one socket is safe.
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
    // RecvFrom() on this socket. See UDPSocket::Shutdown().
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

// ---------------------------------------------------------------------------
// ZDTClientBackend
// ---------------------------------------------------------------------------

ZDTClientBackend::ZDTClientBackend(std::shared_ptr<InetAddress> server_address,
                                   const SessionOptions& options)
    : server_address_(std::move(server_address)), config_(options.zdt),
      session_options_(options) {}

ZDTClientBackend::~ZDTClientBackend() {
  ZNET_LOG_DEBUG("Destructor of the ZDT client backend is called.");
  // Close() first so the FIN still goes out, then join: the loop reads socket_
  // and inbox_, both of which are about to be destroyed.
  Close();
  StopReceiving();
}

Result ZDTClientBackend::BindTo(const InetAddress& address) {
  socket_ = std::make_shared<UDPSocket>();
  Result result = socket_->Open(server_address_->ipv());
  if (result != Result::Success) {
    return result;
  }
  socket_->SetBlocking(false);
  socket_->SetDontFragment(true);  // make the handshake MTU probe meaningful
  result = socket_->Bind(address);
  if (result != Result::Success) {
    return result;
  }
  local_address_ = socket_->local_address();  // resolves an auto-assigned port
  is_bind_ = true;
  return Result::Success;
}

Result ZDTClientBackend::Bind() {
  auto any = InetAddress::from(GetAnyBindAddress(server_address_->ipv()), 0);
  return BindTo(*any);
}

Result ZDTClientBackend::Bind(const std::string& ip, PortNumber port) {
  auto address = InetAddress::from(ip, port);
  if (!address || !address->is_valid()) {
    return Result::InvalidAddress;
  }
  return BindTo(*address);
}

Result ZDTClientBackend::Handshake(ZDTConnection& out) {
  ZDTCookie cookie{};
  uint32_t epoch = 0;
  uint16_t negotiated_mtu = 0;
  uint64_t server_guid = 0;
  bool got_reply1 = false;

  uint8_t buf[ZNET_MAX_BUFFER_SIZE];

  // phase 1: OpenConnectionRequest1 -> OpenConnectionReply1, walking the MTU
  // ladder. The request is padded to the candidate MTU so the padded datagram
  // itself probes the path.
  for (uint16_t rung : config_.mtu_ladder) {
    for (int attempt = 0;
         attempt < config_.handshake_retries_per_rung && !got_reply1; attempt++) {
      Buffer request(Endianness::BigEndian);
      WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest1);
      request.WriteInt<uint8_t>(kZDTProtocolVersion);
      if (request.size() < rung) {
        std::vector<uint8_t> padding(rung - request.size(), 0);
        request.Write(padding.data(), padding.size());
      }
      if (!socket_->SendTo(*server_address_, request.data(), request.size())) {
        break;  // datagram too big for the path (DF set) -> drop to next rung
      }

      auto deadline = steady_clock::now() + config_.handshake_retransmit;
      while (steady_clock::now() < deadline && !got_reply1) {
        size_t len = 0;
        std::shared_ptr<InetAddress> from;
        RecvResult r = socket_->RecvFrom(buf, sizeof(buf), len, from);
        if (r == RecvResult::WouldBlock) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (r == RecvResult::Error) {
          break;
        }
        if (len == 0 || (buf[0] & kFlagOnline)) {
          continue;
        }
        Buffer reply(reinterpret_cast<const char*>(buf), len,
                     Endianness::BigEndian);
        ZDTOfflineMsg id;
        if (!ReadOfflineHeader(reply, id)) {
          continue;
        }
        if (id == ZDTOfflineMsg::OpenConnectionReply1) {
          server_guid = reply.ReadInt<uint64_t>();
          negotiated_mtu = reply.ReadInt<uint16_t>();
          uint8_t cookie_len = reply.ReadInt<uint8_t>();
          if (cookie_len != kZDTCookieLen) {
            continue;
          }
          reply.Read(cookie.data(), cookie.size());
          epoch = reply.ReadInt<uint32_t>();
          got_reply1 = true;
        } else if (id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
          return Result::IncompatibleVersion;
        } else if (id == ZDTOfflineMsg::NoFreeConnections) {
          return Result::ServerFull;
        }
      }
    }
    if (got_reply1) {
      break;
    }
  }
  if (!got_reply1) {
    return Result::Timeout;
  }

  // phase 2: OpenConnectionRequest2 (echo the cookie) -> OpenConnectionReply2.
  for (int attempt = 0; attempt < config_.max_retries; attempt++) {
    Buffer request(Endianness::BigEndian);
    WriteOfflineHeader(request, ZDTOfflineMsg::OpenConnectionRequest2);
    request.WriteInt<uint8_t>(static_cast<uint8_t>(cookie.size()));
    request.Write(cookie.data(), cookie.size());
    request.WriteInt<uint32_t>(epoch);
    request.WriteInetAddress(*server_address_);
    request.WriteInt<uint16_t>(negotiated_mtu);
    request.WriteInt<uint64_t>(guid_);
    socket_->SendTo(*server_address_, request.data(), request.size());

    auto deadline = steady_clock::now() + config_.handshake_retransmit;
    while (steady_clock::now() < deadline) {
      size_t len = 0;
      std::shared_ptr<InetAddress> from;
      RecvResult r = socket_->RecvFrom(buf, sizeof(buf), len, from);
      if (r == RecvResult::WouldBlock) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (r == RecvResult::Error) {
        break;
      }
      if (len == 0 || (buf[0] & kFlagOnline)) {
        continue;
      }
      Buffer reply(reinterpret_cast<const char*>(buf), len, Endianness::BigEndian);
      ZDTOfflineMsg id;
      if (!ReadOfflineHeader(reply, id)) {
        continue;
      }
      if (id == ZDTOfflineMsg::OpenConnectionReply2) {
        uint64_t reply_server_guid = reply.ReadInt<uint64_t>();
        auto external = reply.ReadInetAddress();
        uint16_t mtu = reply.ReadInt<uint16_t>();
        (void)external;
        out.mtu = mtu ? mtu : negotiated_mtu;
        out.local_guid = guid_;
        out.remote_guid = reply_server_guid ? reply_server_guid : server_guid;
        return Result::Success;
      }
      if (id == ZDTOfflineMsg::IncompatibleProtocolVersion) {
        return Result::IncompatibleVersion;
      }
      if (id == ZDTOfflineMsg::NoFreeConnections) {
        return Result::ServerFull;
      }
    }
  }
  return Result::Timeout;
}

Result ZDTClientBackend::Connect() {
  if (client_session_ && client_session_->IsAlive()) {
    return Result::AlreadyConnected;
  }
  if (!server_address_ || !server_address_->is_valid()) {
    return Result::InvalidRemoteAddress;
  }
  if (!is_bind_) {
    ZNET_LOG_ERROR(
        "Cannot connect because the ZDT client is not bound, call Bind() first.");
    return Result::CannotBind;
  }
  guid_ = GenerateGuid();
  ZDTConnection connection;
  Result result = Handshake(connection);
  if (result != Result::Success) {
    ZNET_LOG_ERROR("ZDT handshake with {} failed: {}", server_address_->readable(),
                   GetResultString(result));
    return result;
  }
  ZNET_LOG_DEBUG("ZDT connected to {} (mtu={})", server_address_->readable(),
                 connection.mtu);
  // the receive thread owns the socket from here, so the transport takes its
  // datagrams from the inbox instead of polling alongside it
  inbox_ = std::make_shared<ZDTInbox>();
  auto transport = std::make_unique<ZDTTransportLayer>(
      socket_, server_address_, config_, /*drains_own_socket=*/false, inbox_,
      connection);
  client_session_ = std::make_shared<PeerSession>(
      local_address_, server_address_, std::move(transport), ConnectionType::ZDT,
      /*is_initiator=*/true, /*self_managed=*/false, session_options_);
  // blocking with a timeout: a datagram returns at once, and the timeout only
  // exists so the loop notices shutdown.
  socket_->SetBlocking(true);
  socket_->SetReceiveTimeout(std::chrono::milliseconds(200));
  {
    std::lock_guard<std::mutex> lock(receive_thread_mutex_);
    receiving_ = true;
    receive_thread_ = std::thread([this]() { ReceiveLoop(); });
  }
  return Result::Success;
}

void ZDTClientBackend::ReceiveLoop() {
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  while (receiving_.load(std::memory_order_relaxed)) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    RecvResult result = socket_->RecvFrom(buffer, sizeof(buffer), len, from);
    if (result == RecvResult::WouldBlock) {
      continue;  // receive timeout expired, just re-check the stop flag
    }
    if (result == RecvResult::Error) {
      break;  // socket closed underneath us, shutdown is in progress
    }
    if (len == 0 || !from) {
      continue;
    }
    // one peer, so anything from elsewhere is noise on the port
    if (!(*from == *server_address_)) {
      continue;
    }
    inbox_->Push(buffer, len, config_.max_inbox_datagrams);
    if (on_data_) {
      on_data_();  // the session has work; do not make it wait out its tick
    }
  }
}

void ZDTClientBackend::StopReceiving() {
  std::lock_guard<std::mutex> lock(receive_thread_mutex_);
  receiving_ = false;
  // the loop is parked in recvfrom and would otherwise hold shutdown for a
  // whole receive timeout. only safe once anything outbound has been sent.
  if (socket_ && socket_->IsValid()) {
    ShutdownSocket(socket_->handle());
  }
  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
}

Result ZDTClientBackend::Close() {
  if (!client_session_) {
    if (socket_) {
      socket_->Close();
    }
    return Result::AlreadyClosed;
  }
  return client_session_->Close();
}

void ZDTClientBackend::Update() {}

bool ZDTClientBackend::IsAlive() {
  return client_session_ && client_session_->IsAlive();
}

// ---------------------------------------------------------------------------
// ZDTServerBackend
// ---------------------------------------------------------------------------

ZDTServerBackend::ZDTServerBackend(std::shared_ptr<InetAddress> bind_address,
                                   const SessionOptions& child_options)
    : bind_address_(std::move(bind_address)), config_(child_options.zdt),
      child_session_options_(child_options) {}

ZDTServerBackend::~ZDTServerBackend() {
  ZNET_LOG_DEBUG("Destructor of the ZDT server backend is called.");
  Close();
}

Result ZDTServerBackend::Bind() {
  if (is_bind_) {
    return Result::AlreadyBound;
  }
  if (!bind_address_ || !bind_address_->is_valid()) {
    return Result::InvalidAddress;
  }
  socket_ = std::make_shared<UDPSocket>();
  Result result = socket_->Open(bind_address_->ipv());
  if (result != Result::Success) {
    return result;
  }
  // the receive thread blocks in recvfrom so a datagram wakes it immediately
  // instead of waiting for the next poll. The timeout is only there to give the
  // loop a chance to notice shutdown.
  socket_->SetBlocking(true);
  socket_->SetReceiveTimeout(std::chrono::milliseconds(200));
  socket_->SetReceiveBufferSize(4 * 1024 * 1024);
  result = socket_->Bind(*bind_address_);
  if (result != Result::Success) {
    return result;
  }
  auto local = socket_->local_address();
  if (local) {
    bind_address_ = local;
  }
  // cookie-signing secret + server identity. RAND_bytes needs znet::Init(),
  // which Server::Bind() runs before invoking the backend.
  RAND_bytes(secret_current_.data(), static_cast<int>(secret_current_.size()));
  has_previous_secret_ = false;
  epoch_ = 0;
  last_rotation_ = steady_clock::now();
  server_guid_ = GenerateGuid();
  is_bind_ = true;
  ZNET_LOG_DEBUG("ZDT bind to: {}", bind_address_->readable());
  return Result::Success;
}

Result ZDTServerBackend::Listen() {
  if (is_listening_) {
    return Result::AlreadyListening;
  }
  if (!is_bind_) {
    ZNET_LOG_ERROR(
        "Cannot listen because the ZDT server is not bound, call Bind() first.");
    return Result::NotBound;
  }
  is_listening_ = true;
  receiving_ = true;
  receive_thread_ = std::thread([this]() { ReceiveLoop(); });
  return Result::Success;
}

void ZDTServerBackend::ReceiveLoop() {
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  while (receiving_.load(std::memory_order_relaxed)) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    RecvResult result = socket_->RecvFrom(buffer, sizeof(buffer), len, from);
    if (result == RecvResult::WouldBlock) {
      continue;  // receive timeout expired, just re-check the stop flag
    }
    if (result == RecvResult::Error) {
      break;  // socket closed underneath us, shutdown is in progress
    }
    if (len == 0 || !from) {
      continue;
    }
    RouteDatagram(buffer, len, from);
    if (on_data_) {
      on_data_();  // a session has work; do not make it wait out its tick
    }
  }
}

void ZDTServerBackend::RouteDatagram(uint8_t* data, size_t len,
                                     const std::shared_ptr<InetAddress>& from) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  MaybeRotateSecret();
  if (data[0] & kFlagOnline) {
    auto it = routes_.find(from->readable());
    if (it != routes_.end()) {
      it->second.inbox->Push(data, len, config_.max_inbox_datagrams);
      return;
    }
    // online datagram from an unknown address -> drop.
    ZNET_METRIC(metrics_.zdt.datagrams_unroutable++);
    return;
  }
  Buffer offline(reinterpret_cast<const char*>(data), len,
                 Endianness::BigEndian);
  HandleOffline(offline, from, len);
}

void ZDTServerBackend::MaybeRotateSecret() {
  auto now = steady_clock::now();
  if (now - last_rotation_ < config_.cookie_secret_rotation) {
    return;
  }
  secret_previous_ = secret_current_;
  has_previous_secret_ = true;
  RAND_bytes(secret_current_.data(), static_cast<int>(secret_current_.size()));
  epoch_++;
  last_rotation_ = now;
}

ZDTCookie ZDTServerBackend::CookieFor(const std::string& peer_readable,
                                      uint32_t epoch) const {
  return ComputeCookie(secret_current_.data(), secret_current_.size(),
                       peer_readable, epoch);
}

bool ZDTServerBackend::AllowHandshake(const std::string& peer_readable) {
  auto now = steady_clock::now();
  // keep the table bounded: when it grows large, drop entries whose 1s window
  // has elapsed. This is the only per-source state the server keeps, and it is
  // capped; the stateless cookie remains the primary anti-flood defense.
  if (source_rate_.size() > static_cast<size_t>(config_.max_connections) * 2) {
    for (auto it = source_rate_.begin(); it != source_rate_.end();) {
      if (now - it->second.window_start > std::chrono::seconds(1)) {
        it = source_rate_.erase(it);
      } else {
        ++it;
      }
    }
  }
  SourceRate& entry = source_rate_[peer_readable];
  if (entry.count == 0 || now - entry.window_start > std::chrono::seconds(1)) {
    entry.window_start = now;
    entry.count = 0;
  }
  entry.count++;
  return entry.count <= config_.per_source_handshake_rate;
}

void ZDTServerBackend::HandleOffline(Buffer& buffer,
                                     const std::shared_ptr<InetAddress>& from,
                                     size_t datagram_size) {
  ZDTOfflineMsg id;
  if (!ReadOfflineHeader(buffer, id)) {
    return;
  }
  const std::string key = from->readable();
  if (!AllowHandshake(key)) {
    ZNET_METRIC(metrics_.zdt.rate_limited++);
    return;  // per-source handshake rate exceeded -> drop silently
  }

  if (id == ZDTOfflineMsg::OpenConnectionRequest1) {
    ZNET_METRIC(metrics_.zdt.handshakes_started++);
    uint8_t version = buffer.ReadInt<uint8_t>();
    if (version != kZDTProtocolVersion) {
      ZNET_METRIC(metrics_.zdt.handshakes_rejected++);
      Buffer out(Endianness::BigEndian);
      WriteOfflineHeader(out, ZDTOfflineMsg::IncompatibleProtocolVersion);
      out.WriteInt<uint8_t>(kZDTProtocolVersion);
      out.WriteInt<uint64_t>(server_guid_);
      socket_->SendTo(*from, out.data(), out.size());
      return;
    }
    // allocate nothing here. The received size is the MTU the path carried,
    // capped to the top ladder rung.
    uint16_t mtu =
        static_cast<uint16_t>(std::min<size_t>(datagram_size,
                                               config_.mtu_ladder.front()));
    ZDTCookie cookie = CookieFor(key, epoch_);
    Buffer out(Endianness::BigEndian);
    WriteOfflineHeader(out, ZDTOfflineMsg::OpenConnectionReply1);
    out.WriteInt<uint64_t>(server_guid_);
    out.WriteInt<uint16_t>(mtu);
    out.WriteInt<uint8_t>(static_cast<uint8_t>(cookie.size()));
    out.Write(cookie.data(), cookie.size());
    out.WriteInt<uint32_t>(epoch_);
    socket_->SendTo(*from, out.data(), out.size());
    return;
  }

  if (id == ZDTOfflineMsg::OpenConnectionRequest2) {
    uint8_t cookie_len = buffer.ReadInt<uint8_t>();
    if (cookie_len != kZDTCookieLen) {
      return;
    }
    ZDTCookie cookie{};
    buffer.Read(cookie.data(), cookie.size());
    uint32_t epoch = buffer.ReadInt<uint32_t>();
    auto target = buffer.ReadInetAddress();
    (void)target;
    uint16_t mtu = buffer.ReadInt<uint16_t>();
    uint64_t client_guid = buffer.ReadInt<uint64_t>();

    // validate the cookie against the source address (return-routability).
    bool valid = false;
    if (epoch == epoch_) {
      valid = ConstTimeEqual(cookie, CookieFor(key, epoch));
    } else if (has_previous_secret_ && epoch == epoch_ - 1) {
      valid = ConstTimeEqual(
          cookie, ComputeCookie(secret_previous_.data(),
                                secret_previous_.size(), key, epoch));
    }
    if (!valid) {
      ZNET_METRIC(metrics_.zdt.cookies_rejected++);
      // silent drop: never reply to an unvalidated address.
      return;
    }

    auto reply2 = [&]() {
      Buffer out(Endianness::BigEndian);
      WriteOfflineHeader(out, ZDTOfflineMsg::OpenConnectionReply2);
      out.WriteInt<uint64_t>(server_guid_);
      out.WriteInetAddress(*from);
      out.WriteInt<uint16_t>(mtu);
      socket_->SendTo(*from, out.data(), out.size());
    };

    // duplicate Request2 (Reply2 was lost): re-answer idempotently.
    auto existing = routes_.find(key);
    if (existing != routes_.end() && !existing->second.session.expired()) {
      reply2();
      return;
    }
    if (static_cast<int>(routes_.size()) >= config_.max_connections) {
      ZNET_METRIC(metrics_.zdt.handshakes_rejected++);
      Buffer out(Endianness::BigEndian);
      WriteOfflineHeader(out, ZDTOfflineMsg::NoFreeConnections);
      out.WriteInt<uint64_t>(server_guid_);
      socket_->SendTo(*from, out.data(), out.size());
      return;
    }

    // address proven, allocate the session now.
    auto inbox = std::make_shared<ZDTInbox>();
    ZDTConnection connection;
    connection.mtu = mtu ? mtu : config_.mtu_ladder.back();
    connection.local_guid = server_guid_;
    connection.remote_guid = client_guid;
    auto transport = std::make_unique<ZDTTransportLayer>(
        socket_, from, config_, /*drains_own_socket=*/false, inbox, connection);
    auto session = std::make_shared<PeerSession>(
        bind_address_, from, std::move(transport), ConnectionType::ZDT,
        /*is_initiator=*/false, /*self_managed=*/false,
        child_session_options_);

    Route route;
    route.session = session;
    route.inbox = inbox;
    route.peer = from;
    route.remote_guid = client_guid;
    routes_[key] = std::move(route);
    ZNET_METRIC(metrics_.connections_accepted++);
    pending_accept_.push_back(session);
    reply2();
    ZNET_LOG_DEBUG("ZDT accepted handshake from {} (mtu={})", key, connection.mtu);
    return;
  }
}

void ZDTServerBackend::StopReceiving() {
  std::lock_guard<std::mutex> lock(receive_thread_mutex_);
  receiving_ = false;
  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
}

Result ZDTServerBackend::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_listening_ && !is_bind_) {
      return Result::AlreadyStopped;
    }
    is_listening_ = false;
    is_bind_ = false;
  }
  // stop and join before touching the tables, otherwise the receive thread is
  // still routing into them. Joined outside every lock it might be waiting on,
  // and harmless if the shutdown path already did it.
  StopReceiving();
  std::lock_guard<std::mutex> lock(state_mutex_);
  routes_.clear();
  pending_accept_.clear();
  source_rate_.clear();
  if (socket_) {
    socket_->Close();
  }
  return Result::Success;
}

void ZDTServerBackend::Update() {}

std::shared_ptr<PeerSession> ZDTServerBackend::Accept() {
  if (!is_listening_) {
    return nullptr;
  }
  // the receive thread fills routes_/pending_accept_, this only harvests them.
  std::lock_guard<std::mutex> lock(state_mutex_);
  // reap routes whose sessions have been destroyed.
  for (auto it = routes_.begin(); it != routes_.end();) {
    if (it->second.session.expired()) {
      it = routes_.erase(it);
    } else {
      ++it;
    }
  }
  ZNET_METRIC(metrics_.connections_active = routes_.size());
  if (pending_accept_.empty()) {
    return nullptr;
  }
  auto session = pending_accept_.front();
  pending_accept_.pop_front();
  return session;
}

void ZDTServerBackend::AcceptAndReject() {
  // nothing to reject at the socket level for UDP; unvalidated handshakes are
  // simply not promoted to sessions.
}

bool ZDTServerBackend::IsAlive() {
  return is_listening_;
}

}  // namespace backends
}  // namespace znet
