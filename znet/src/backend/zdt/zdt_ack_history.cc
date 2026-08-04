//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/backends/zdt/zdt_ack_history.h"

namespace znet {
namespace backends {

namespace {

// bit b of the history means "packet_seq (highest_ - b) arrived".
bool HistoryBit(const std::array<uint64_t, kZDTAckHistoryWords>& bits, size_t b) {
  return (bits[b / 64] >> (b % 64)) & 1u;
}

void SetHistoryBit(std::array<uint64_t, kZDTAckHistoryWords>& bits, size_t b) {
  bits[b / 64] |= (uint64_t{1} << (b % 64));
}

// the newest sequence advanced by `n`, so every recorded packet is now `n`
// further back: bit b becomes bit b+n, and the newly exposed low bits are
// unknown.
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

void ZDTAckHistory::Record(WireSeq packet_seq) {
  if (!has_any_) {
    has_any_ = true;
    highest_ = packet_seq;
    bits_.fill(0);
    SetHistoryBit(bits_, 0);  // bit 0 is highest_ itself
    valid_bits_ = 1;
    return;
  }
  if (SeqGreater(packet_seq, highest_)) {
    const uint16_t shift = static_cast<uint16_t>(packet_seq - highest_);
    ShiftHistoryUp(bits_, shift);
    highest_ = packet_seq;
    SetHistoryBit(bits_, 0);
    valid_bits_ += shift;
    if (valid_bits_ > kZDTAckHistoryBits) {
      valid_bits_ = kZDTAckHistoryBits;
    }
  } else if (SeqLess(packet_seq, highest_)) {
    const uint16_t back = static_cast<uint16_t>(highest_ - packet_seq);
    if (back < kZDTAckHistoryBits) {
      SetHistoryBit(bits_, back);  // a late arrival, not a gap
    }
  }
  // equal -> duplicate of the current highest; nothing to do
}

// walks the received history backwards from `ack` and run-length encodes it.
// the first block always starts on a received run, because bit 0 is `ack`
// itself. Trailing blocks that describe only missing packets are dropped: the
// sender learns nothing from "everything older than this is still missing",
// and a later acknowledgment will carry them once something in between lands.
void ZDTAckHistory::Fill(ZDTHeader& header, size_t max_blocks,
                         size_t reportable_cap) const {
  header.ack = highest_;
  header.block_count = 0;
  if (!has_any_ || max_blocks == 0) {
    return;
  }
  if (max_blocks > kZDTMaxAckBlocks) {
    max_blocks = kZDTMaxAckBlocks;
  }
  // bounded by what the peer has actually sent, and by how far back is worth
  // describing. Truncating is safe where hiding a gap is not: runs are
  // positional, so folding one into an ack run would falsely retire a message.
  size_t limit = valid_bits_;
  if (limit > reportable_cap) {
    limit = reportable_cap;
  }
  size_t b = 0;
  while (b < limit && header.block_count < max_blocks) {
    size_t acks = 0;
    while (b < limit && acks < 255 && HistoryBit(bits_, b)) {
      acks++;
      b++;
    }
    size_t nacks = 0;
    while (b < limit && nacks < 255 && !HistoryBit(bits_, b)) {
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
}

}  // namespace backends
}  // namespace znet
