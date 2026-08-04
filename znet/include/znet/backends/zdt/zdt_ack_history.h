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
// What a ZDT receiver remembers about which datagrams arrived, and how it
// encodes that into the ack blocks of an outgoing header. Purely a receiver's
// bookkeeping: it holds no sockets, no timers and no connection state, so it
// can be exercised without either.
//

#ifndef ZNET_BACKENDS_ZDT_ZDT_ACK_HISTORY_H_
#define ZNET_BACKENDS_ZDT_ZDT_ACK_HISTORY_H_

#include "znet/backends/zdt/zdt_wire.h"
#include "znet/precompiled.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace znet {
namespace backends {

/**
 * @brief A sliding record of which packet sequences arrived, newest first.
 *
 * Bit i means "the datagram `highest() - i` arrived". Bit 0 is `highest()`
 * itself and is always set once anything has been recorded. The window slides
 * forward as newer sequences arrive; anything that falls off the back is
 * forgotten, which is safe because the peer cannot still be waiting on it.
 */
class ZDTAckHistory {
 public:
  /** @brief Folds one arrival in. Out-of-order and duplicate arrivals are
   *  handled: a late one sets its bit, a duplicate changes nothing. */
  void Record(WireSeq packet_seq);

  /**
   * @brief Run-length encodes the history into `header`'s ack blocks.
   *
   * Walks backwards from the newest sequence, alternating runs of arrived and
   * missing. The first block always starts on an arrived run, because bit 0 is
   * the newest sequence itself.
   *
   * @param header         receives `ack`, `blocks` and `block_count`.
   * @param max_blocks     what fits in the datagram, clamped to
   *                       kZDTMaxAckBlocks.
   * @param reportable_cap how far back is worth describing, in sequences.
   *                       Anything older the peer has already seen acked or it
   *                       could not have kept sending.
   */
  void Fill(ZDTHeader& header, size_t max_blocks, size_t reportable_cap) const;

  /** @brief Whether anything has been recorded yet. Until it has, there is no
   *  meaningful `ack` to send. */
  ZNET_NODISCARD bool has_any() const { return has_any_; }

  /** @brief The newest sequence recorded. */
  ZNET_NODISCARD WireSeq highest() const { return highest_; }

 private:
  WireSeq highest_ = 0;
  // bit i means "packet_seq (highest_ - i) arrived"
  std::array<uint64_t, kZDTAckHistoryWords> bits_{};
  // how much of bits_ describes sequences the peer has actually sent. Zeroes
  // past this are history never observed, and reporting them would invent
  // losses on a new connection.
  size_t valid_bits_ = 0;
  bool has_any_ = false;
};

}  // namespace backends
}  // namespace znet


#endif  // ZNET_BACKENDS_ZDT_ZDT_ACK_HISTORY_H_
