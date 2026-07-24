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
  buffer.WriteInt<uint8_t>(header.channel);
  buffer.WriteInt<uint16_t>(header.packet_seq);
  buffer.WriteInt<uint16_t>(header.ack);
  buffer.WriteInt<uint32_t>(header.ack_bits);
  buffer.WriteInt<uint16_t>(header.message_seq);
  if (header.flags & kFlagFragment) {
    buffer.WriteInt<uint8_t>(header.frag_index);
    buffer.WriteInt<uint8_t>(header.frag_count);
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
  out_header.channel = buffer.ReadInt<uint8_t>();
  out_header.packet_seq = buffer.ReadInt<uint16_t>();
  out_header.ack = buffer.ReadInt<uint16_t>();
  out_header.ack_bits = buffer.ReadInt<uint32_t>();
  out_header.message_seq = buffer.ReadInt<uint16_t>();
  if (out_header.flags & kFlagFragment) {
    if (buffer.readable_bytes() < 2) {
      return false;
    }
    out_header.frag_index = buffer.ReadInt<uint8_t>();
    out_header.frag_count = buffer.ReadInt<uint8_t>();
  }
  return true;
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
  rto_ = std::clamp(std::chrono::milliseconds(200), config_.rto_min,
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
  if (outbound_.size() >= config_.max_outbound_messages) {
    // refusing is the backpressure signal to the caller.
    ZNET_LOG_WARN("ZDT: outbound queue full ({}), dropping packet!",
                  outbound_.size());
    return false;
  }
  outbound_.push_back(QueuedOut{std::move(buffer), options});
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
  out.common.outbound_queued = static_cast<uint32_t>(outbound_.size());
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
  std::deque<std::vector<uint8_t>> local;
  inbox_->Drain(local);
  for (auto& raw : local) {
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
    RecordRemoteSeq(header.packet_seq);  // build our outgoing (ack, ack_bits)
    ProcessAcks(header);                 // consume the peer's acks
    if (header.flags & kFlagFin) {
      is_closed_ = true;
      if (drains_own_socket_ && socket_) {
        socket_->Close();
      }
      return;
    }
    if (header.flags & kFlagPing) {
      SendControl(kFlagPong);
      continue;
    }
    if (header.flags & kFlagData) {
      needs_ack_ = true;  // we owe the sender an ack for this message
      const char* payload = buffer.read_cursor_data();
      size_t payload_len = buffer.readable_bytes();
      if (header.flags & kFlagFragment) {
        OnDataFragment(header, reinterpret_cast<const uint8_t*>(payload),
                       payload_len);
      } else {
        DeliverMessage(header, std::make_shared<Buffer>(payload, payload_len));
      }
    }
    // kFlagPong / ack-only carry no payload; their acks were handled above.
  }
}

void ZDTTransportLayer::FlushOutbound() {
  uint16_t mtu =
      connection_.mtu != 0 ? connection_.mtu : config_.mtu_ladder.back();
  if (mtu < kZDTFragHeaderSize + 1) {
    mtu = static_cast<uint16_t>(kZDTFragHeaderSize + 1);
  }
  const size_t unfrag_capacity = mtu - kZDTHeaderSize;
  const size_t frag_capacity = mtu - kZDTFragHeaderSize;

  // bound the gap between the newest send and the oldest unacked message to half
  // a wire period, so a late retransmit can never reconstruct onto a live message.
  // cwnd normally keeps the gap far below this.
  constexpr SequenceId kMaxSeqGap = (SequenceId{1} << 16) / 2;

  while (!outbound_.empty()) {
    const bool next_reliable = outbound_.front().options.GetOr<ReliableKey>(true);
    // only reliable traffic is windowed; unreliable sends are never held back.
    if (next_reliable &&
        unacked_.size() >= static_cast<size_t>(config_.cwnd)) {
      break;
    }
    if (next_reliable && !unacked_.empty()) {
      uint8_t next_channel = outbound_.front().options.GetOr<ChannelKey>(0);
      auto oldest = unacked_.lower_bound(MsgKey{next_channel, true, 0, 0});
      if (oldest != unacked_.end() && oldest->first.channel == next_channel &&
          oldest->first.reliable &&
          channels_[next_channel].rel_send - oldest->first.message_seq >=
              kMaxSeqGap) {
        break;  // stalled until the oldest unacked message is retired
      }
    }
    QueuedOut queued = std::move(outbound_.front());
    outbound_.pop_front();

    uint8_t channel = queued.options.GetOr<ChannelKey>(0);
    bool reliable = queued.options.GetOr<ReliableKey>(true);
    bool ordered = queued.options.GetOr<OrderedKey>(true);
    uint8_t data_flags = kFlagData;
    if (reliable) {
      data_flags |= kFlagReliable;
    }
    if (ordered) {
      data_flags |= kFlagOrdered;
    }
    // reliable and unreliable messages advance independent per-channel sequence
    // spaces so they can coexist on one channel.
    ChannelState& state = channels_[channel];
    SequenceId message_seq = reliable ? state.rel_send++ : state.unrel_send++;

    const size_t read_base = queued.payload->read_cursor();
    const char* data = queued.payload->data() + read_base;
    const size_t total = queued.payload->size();
    auto now = steady_clock::now();

    // small enough for one datagram.
    if (total <= unfrag_capacity) {
      MsgKey key{channel, reliable, message_seq, 0};
      WireSeq packet = SendDatagram(data_flags, channel, message_seq, 0, 1, data,
                                    total, reliable, key);
      if (reliable) {
        unacked_[key] = OutReliable{
            queued.payload, read_base, total,
            channel,        static_cast<WireSeq>(message_seq), 0,
            1,              data_flags, now,
            1,              MakeLog(packet)};
      }
      continue;
    }

    // fragment into MTU-sized pieces; each fragment is its own reliable unit.
    size_t fragment_count = (total + frag_capacity - 1) / frag_capacity;
    if (fragment_count > 255) {
      ZNET_LOG_ERROR(
          "ZDT: message of {} bytes needs {} fragments (>255), dropping.", total,
          fragment_count);
      continue;
    }
    uint8_t frag_count = static_cast<uint8_t>(fragment_count);
    uint8_t frag_flags = static_cast<uint8_t>(data_flags | kFlagFragment);
    for (uint8_t i = 0; i < frag_count; i++) {
      size_t rel_offset = static_cast<size_t>(i) * frag_capacity;
      size_t length = std::min(frag_capacity, total - rel_offset);
      MsgKey key{channel, reliable, message_seq, i};
      WireSeq packet =
          SendDatagram(frag_flags, channel, message_seq, i, frag_count,
                       data + rel_offset, length, reliable, key);
      if (reliable) {
        unacked_[key] = OutReliable{queued.payload,
                                    read_base + rel_offset,
                                    length,
                                    channel,
                                    static_cast<WireSeq>(message_seq),
                                    i,
                                    frag_count,
                                    frag_flags,
                                    now,
                                    1,
                                    MakeLog(packet)};
      }
    }
  }
}

void ZDTTransportLayer::SendControl(uint8_t flags) {
  SendDatagram(flags, 0, 0, 0, 1, nullptr, 0, false, MsgKey{});
}

WireSeq ZDTTransportLayer::SendDatagram(uint8_t extra_flags, uint8_t channel,
                                        SequenceId message_seq,
                                        uint8_t frag_index, uint8_t frag_count,
                                        const char* payload, size_t payload_len,
                                        bool reliable,
                                        const MsgKey& reliable_key) {
  if (!socket_ || !peer_) {
    return next_packet_seq_;
  }
  ZDTHeader header;
  header.flags = static_cast<uint8_t>(kFlagOnline | extra_flags);
  header.channel = channel;
  header.packet_seq = next_packet_seq_++;
  if (next_packet_seq_ == 0) {
    next_packet_seq_ = 1;  // skip the reserved sentinel on wraparound
  }
  header.message_seq = static_cast<WireSeq>(message_seq);
  header.frag_index = frag_index;
  header.frag_count = frag_count;
  FillAck(header);

  Buffer datagram(Endianness::BigEndian);
  WriteZDTHeader(datagram, header);  // writes frag fields iff kFlagFragment set
  if (payload && payload_len > 0) {
    datagram.Write(payload, payload_len);
  }
  socket_->SendTo(*peer_, datagram.data(), datagram.size());
  ZNET_METRIC(metrics_.zdt.datagrams_sent++);
  ZNET_METRIC(metrics_.common.wire_bytes_sent += datagram.size());

  last_send_ = steady_clock::now();
  needs_ack_ = false;  // this datagram piggybacked our current ack
  sent_packets_[header.packet_seq] = SentInfo{last_send_, reliable, reliable_key};
  return header.packet_seq;
}

void ZDTTransportLayer::FillAck(ZDTHeader& header) const {
  header.ack = remote_ack_seq_;
  header.ack_bits = remote_ack_bits_;
}

void ZDTTransportLayer::RecordRemoteSeq(WireSeq packet_seq) {
  if (!has_remote_seq_) {
    has_remote_seq_ = true;
    remote_ack_seq_ = packet_seq;
    remote_ack_bits_ = 0;
    return;
  }
  if (SeqGreater(packet_seq, remote_ack_seq_)) {
    uint16_t shift = static_cast<uint16_t>(packet_seq - remote_ack_seq_);
    if (shift >= 32) {
      remote_ack_bits_ = 0;
    } else {
      remote_ack_bits_ <<= shift;
      remote_ack_bits_ |= (1u << (shift - 1));  // old highest -> a set bit
    }
    remote_ack_seq_ = packet_seq;
  } else if (SeqLess(packet_seq, remote_ack_seq_)) {
    uint16_t back = static_cast<uint16_t>(remote_ack_seq_ - packet_seq);
    if (back >= 1 && back <= 32) {
      remote_ack_bits_ |= (1u << (back - 1));
    }
  }
  // equal -> duplicate of the current highest; nothing to do
}

void ZDTTransportLayer::ProcessAcks(const ZDTHeader& header) {
  AckPacket(header.ack);
  for (int i = 0; i < 32; i++) {
    if (header.ack_bits & (1u << i)) {
      AckPacket(static_cast<uint16_t>(header.ack - 1 - i));
    }
  }
}

void ZDTTransportLayer::AckPacket(WireSeq packet_seq) {
  if (packet_seq == 0) {
    return;  // reserved sentinel: the peer had nothing to acknowledge yet
  }
  auto it = sent_packets_.find(packet_seq);
  if (it == sent_packets_.end()) {
    return;
  }
  SentInfo info = it->second;
  sent_packets_.erase(it);
  // packet_seq is unique per transmission, so there is no Karn ambiguity.
  UpdateRtt(steady_clock::now() - info.send_time);
  if (!info.reliable) {
    return;
  }
  // retire the message and drop its other transmissions, so no sent_packets_
  // entry outlives the unacked_ entry that owns it.
  auto msg = unacked_.find(info.reliable_key);
  if (msg != unacked_.end()) {
    for (WireSeq seq : msg->second.packets) {
      sent_packets_.erase(seq);
    }
    unacked_.erase(msg);
  }
}

void ZDTTransportLayer::UpdateRtt(std::chrono::steady_clock::duration sample) {
  double ms = std::chrono::duration<double, std::milli>(sample).count();
  if (ms < 0.0) {
    return;
  }
  if (!has_rtt_) {
    has_rtt_ = true;
    srtt_ms_ = ms;
    rttvar_ms_ = ms / 2.0;
  } else {
    rttvar_ms_ = 0.75 * rttvar_ms_ + 0.25 * std::fabs(srtt_ms_ - ms);
    srtt_ms_ = 0.875 * srtt_ms_ + 0.125 * ms;
  }
  auto rto = std::chrono::milliseconds(
      static_cast<long long>(srtt_ms_ + 4.0 * rttvar_ms_));
  rto_ = std::clamp(rto, config_.rto_min, config_.rto_max);
}

void ZDTTransportLayer::RetransmitUnacked() {
  auto now = steady_clock::now();
  for (auto& entry : unacked_) {
    OutReliable& msg = entry.second;
    int backoff = std::min(msg.send_count - 1, 6);
    auto threshold = std::min(rto_ * (1 << backoff), config_.rto_max);
    if (now - msg.last_send < threshold) {
      continue;
    }
    if (msg.send_count >= config_.max_retries) {
      ZNET_LOG_WARN("ZDT: reliable message to {} exceeded {} retries, closing.",
                    peer_->readable(), config_.max_retries);
      Close();
      return;
    }
    WireSeq packet = SendDatagram(
        msg.data_flags, msg.channel, entry.first.message_seq, msg.frag_index,
        msg.frag_count, msg.message->data() + msg.offset, msg.length, true,
        entry.first);
    msg.packets.Add(packet);
    ZNET_METRIC(metrics_.zdt.retransmits++);
    msg.last_send = now;
    msg.send_count++;
  }
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

SequenceId ZDTTransportLayer::ReconstructSeqFor(const ZDTHeader& header) {
  ChannelState& channel = channels_[header.channel];
  if (header.flags & kFlagReliable) {
    return ReconstructSeq(header.message_seq, channel.rel_expected);
  }
  SequenceId expected =
      channel.unrel_started ? channel.unrel_last + 1 : SequenceId{0};
  return ReconstructSeq(header.message_seq, expected);
}

void ZDTTransportLayer::DeliverMessage(const ZDTHeader& header,
                                       std::shared_ptr<Buffer> payload) {
  const bool reliable = header.flags & kFlagReliable;
  const bool ordered = header.flags & kFlagOrdered;
  const SequenceId seq = ReconstructSeqFor(header);

  // unreliable + unordered: no state, and nothing retransmits, so no dedup.
  if (!reliable && !ordered) {
    ready_.push_back(std::move(payload));
    return;
  }

  ChannelState& channel = channels_[header.channel];

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
  FlushOutbound();
  PruneSentPackets();
  PruneReassembly();
  // if we still owe an ack and nothing above carried it, send a standalone one.
  if (needs_ack_) {
    SendControl(0);
  }
}

Result ZDTTransportLayer::Close(CloseOptions options) {
  (void)options;
  if (is_closed_) {
    return Result::AlreadyDisconnected;
  }
  SendControl(kFlagFin);  // best-effort graceful close
  is_closed_ = true;
  if (drains_own_socket_ && socket_) {
    socket_->Close();
  }
  return Result::Success;
}

void ZDTTransportLayer::OnDataFragment(const ZDTHeader& header,
                                       const uint8_t* data, size_t len) {
  if (header.frag_count == 0 || header.frag_index >= header.frag_count) {
    return;  // malformed
  }
  bool reliable = header.flags & kFlagReliable;
  MsgKey key{header.channel, reliable, ReconstructSeqFor(header), 0};
  if (reassembly_.find(key) == reassembly_.end() &&
      reassembly_.size() >= config_.max_reassemblies) {
    ZNET_METRIC(metrics_.zdt.reassemblies_dropped++);
    return;  // too many partial messages in flight; drop until some time out
  }
  Reassembly& assembly = reassembly_[key];
  if (assembly.frag_count == 0) {
    assembly.frag_count = header.frag_count;
    assembly.first_seen = steady_clock::now();
  }
  if (assembly.frag_count != header.frag_count) {
    return;  // inconsistent fragment count for this message
  }
  assembly.fragments[header.frag_index].assign(data, data + len);  // dedups
  if (assembly.fragments.size() != assembly.frag_count) {
    return;  // still incomplete
  }
  auto full = std::make_shared<Buffer>();
  for (auto& fragment : assembly.fragments) {  // std::map -> ascending index
    if (!fragment.second.empty()) {
      full->Write(fragment.second.data(), fragment.second.size());
    }
  }
  reassembly_.erase(key);
  DeliverMessage(header, std::move(full));
}

void ZDTTransportLayer::PruneReassembly() {
  auto now = steady_clock::now();
  for (auto it = reassembly_.begin(); it != reassembly_.end();) {
    if (now - it->second.first_seen > config_.reassembly_timeout) {
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
    : server_address_(std::move(server_address)), config_(options.zdt) {}

ZDTClientBackend::~ZDTClientBackend() {
  ZNET_LOG_DEBUG("Destructor of the ZDT client backend is called.");
  Close();
}

Result ZDTClientBackend::Bind() {
  socket_ = std::make_shared<UDPSocket>();
  Result result = socket_->Open(server_address_->ipv());
  if (result != Result::Success) {
    return result;
  }
  socket_->SetBlocking(false);
  socket_->SetDontFragment(true);  // make the handshake MTU probe meaningful
  auto any = InetAddress::from(GetAnyBindAddress(server_address_->ipv()), 0);
  result = socket_->Bind(*any);
  if (result != Result::Success) {
    return result;
  }
  local_address_ = socket_->local_address();
  is_bind_ = true;
  return Result::Success;
}

Result ZDTClientBackend::Bind(const std::string& ip, PortNumber port) {
  socket_ = std::make_shared<UDPSocket>();
  Result result = socket_->Open(server_address_->ipv());
  if (result != Result::Success) {
    return result;
  }
  socket_->SetBlocking(false);
  socket_->SetDontFragment(true);  // make the handshake MTU probe meaningful
  local_address_ = InetAddress::from(ip, port);
  if (!local_address_ || !local_address_->is_valid()) {
    return Result::InvalidAddress;
  }
  result = socket_->Bind(*local_address_);
  if (result != Result::Success) {
    return result;
  }
  local_address_ = socket_->local_address();
  is_bind_ = true;
  return Result::Success;
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
  auto transport = std::make_unique<ZDTTransportLayer>(
      socket_, server_address_, config_, /*drains_own_socket=*/true,
      /*inbox=*/nullptr, connection);
  client_session_ = std::make_shared<PeerSession>(
      local_address_, server_address_, std::move(transport), ConnectionType::ZDT,
      /*is_initiator=*/true);
  return Result::Success;
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
    : bind_address_(std::move(bind_address)), config_(child_options.zdt) {}

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
  socket_->SetBlocking(false);
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
  return Result::Success;
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
        /*is_initiator=*/false);

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

void ZDTServerBackend::DrainAndRoute() {
  MaybeRotateSecret();
  uint8_t buffer[ZNET_MAX_BUFFER_SIZE];
  while (true) {
    size_t len = 0;
    std::shared_ptr<InetAddress> from;
    RecvResult result = socket_->RecvFrom(buffer, sizeof(buffer), len, from);
    if (result != RecvResult::Received) {
      break;
    }
    if (len == 0 || !from) {
      continue;
    }
    if (buffer[0] & kFlagOnline) {
      auto it = routes_.find(from->readable());
      if (it != routes_.end()) {
        it->second.inbox->Push(buffer, len, config_.max_inbox_datagrams);
      }
      ZNET_METRIC(metrics_.zdt.datagrams_unroutable++);
      // online datagram from an unknown address -> drop.
      continue;
    }
    Buffer offline(reinterpret_cast<const char*>(buffer), len,
                   Endianness::BigEndian);
    HandleOffline(offline, from, len);
  }
}

Result ZDTServerBackend::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_listening_ && !is_bind_) {
    return Result::AlreadyStopped;
  }
  is_listening_ = false;
  is_bind_ = false;
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
  DrainAndRoute();
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
