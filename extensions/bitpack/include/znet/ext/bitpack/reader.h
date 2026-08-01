//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <cstdint>

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/ext/bitpack/bits.h"

namespace znet {
namespace ext {

/**
 * @brief Reads sub-byte fields written by BitWriter.
 *
 * The mirror of BitWriter: same calls in the same order with the same bounds.
 * Nothing in the stream is self-describing, so a reader that disagrees with
 * the writer about a field's width desynchronises from that point on -- which
 * is exactly why the ranged calls take their bounds rather than a bit count
 * wherever they can, since bounds are far harder to get subtly wrong than a
 * hand-counted width.
 *
 * @code
 *   const uint32_t entity_id = buffer->ReadInt<uint32_t>();
 *   {
 *     BitReader bits(*buffer);
 *     packet->firing = bits.ReadBool();
 *     packet->ammo   = bits.ReadUIntRanged(0, 200);
 *     packet->health = bits.ReadIntRanged(-50, 100);
 *     packet->yaw    = bits.ReadFloatRanged(-3.15f, 3.15f, 12);
 *     if (!bits.ok()) return nullptr;   // ran off the end of the packet
 *   }
 *   packet->name = buffer->ReadString();
 * @endcode
 *
 * Bytes leave the buffer only when a field needs them, so the buffer's read
 * cursor is correct for ordinary Buffer reads the moment the reader has
 * consumed a whole number of bytes -- which is where a matching BitWriter's
 * Flush leaves it.
 *
 * Every ranged read is clamped to the bounds it was given. A decoded value can
 * be wrong if the packet was malformed, but it is never out of range, so it is
 * safe to use as an array index or a loop count without a second check.
 */
class BitReader {
 public:
  explicit BitReader(Buffer& buffer) : buffer_(buffer) {}

  BitReader(const BitReader&) = delete;
  BitReader& operator=(const BitReader&) = delete;

  /** @brief Reads @p bits bits, least significant first. Returns 0 past the end. */
  uint32_t ReadBits(unsigned bits) {
    if (bits == 0) {
      return 0;
    }
    if (ZNET_UNLIKELY(bits > kMaxBitsPerCall)) ZNET_UNLIKELY_ATTR {
      bits = kMaxBitsPerCall;
      ok_ = false;
    }
    while (scratch_bits_ < bits) {
      if (!Refill()) {
        return 0;
      }
    }
    const uint32_t result =
        bits >= 32 ? static_cast<uint32_t>(scratch_ & 0xFFFFFFFFu)
                   : static_cast<uint32_t>(scratch_ &
                                           ((UINT32_C(1) << bits) - 1));
    scratch_ >>= bits;
    scratch_bits_ -= bits;
    bits_read_ += bits;
    return result;
  }

  /** @brief ReadBits for fields wider than 32 bits. */
  uint64_t ReadBits64(unsigned bits) {
    if (bits <= kMaxBitsPerCall) {
      return ReadBits(bits);
    }
    if (ZNET_UNLIKELY(bits > 64)) ZNET_UNLIKELY_ATTR {
      bits = 64;
      ok_ = false;
    }
    const uint64_t low = ReadBits(32);
    const uint64_t high = ReadBits(bits - 32);
    return low | (high << 32);
  }

  /** @brief One bit. */
  bool ReadBool() { return ReadBits(1) != 0; }

  /** @brief Reads a value written by WriteUIntRanged with the same bounds. */
  uint64_t ReadUIntRanged(uint64_t min, uint64_t max) {
    const unsigned bits = BitsForRange(min, max);
    if (bits == 0) {
      return min;
    }
    const uint64_t span = max - min;
    const uint64_t offset = ReadBits64(bits);
    // the field is wide enough to express codes past the top of the range, so
    // a malformed packet can send one. Clamping here is what lets callers
    // treat the result as genuinely within [min, max].
    return offset >= span ? max : min + offset;
  }

  /** @brief Reads a value written by WriteIntRanged with the same bounds. */
  int64_t ReadIntRanged(int64_t min, int64_t max) {
    const unsigned bits = BitsForSignedRange(min, max);
    if (bits == 0) {
      return min;
    }
    const uint64_t span =
        static_cast<uint64_t>(max) - static_cast<uint64_t>(min);
    const uint64_t offset = ReadBits64(bits);
    if (offset >= span) {
      return max;
    }
    // the sum is within [min, max] by construction, so the round trip through
    // unsigned is exact on any two's-complement target
    return static_cast<int64_t>(static_cast<uint64_t>(min) + offset);
  }

  /** @brief Reads a value written by WriteFloatRanged with the same bounds and width. */
  float ReadFloatRanged(float min, float max, unsigned bits) {
    if (bits == 0) {
      return min;
    }
    if (bits > kMaxBitsPerCall) {
      bits = kMaxBitsPerCall;
      ok_ = false;
    }
    return detail::DequantizeFloat(ReadBits(bits), min, max, bits);
  }

  /** @brief Reads a value written by WriteVarUInt. */
  uint64_t ReadVarUInt() {
    uint64_t value = 0;
    unsigned shift = 0;
    for (;;) {
      const uint64_t nibble = ReadBits(4);
      if (shift < 64) {
        value |= nibble << shift;
      }
      const bool more = ReadBool();
      shift += 4;
      // bounded whatever the stream says: 16 groups covers a full uint64, and
      // a truncated stream stops here because ReadBool has gone false
      if (!more || shift >= 64 || !ok_) {
        return value;
      }
    }
  }

  /** @brief Discards bits up to the next byte boundary. */
  void Align() {
    const unsigned pad = static_cast<unsigned>((8u - (bits_read_ % 8u)) % 8u);
    if (pad != 0) {
      ReadBits(pad);
    }
  }

  /**
   * @brief False once a read ran past the end of the buffer.
   *
   * Reads after that point return zero rather than garbage. The buffer also
   * records ReadOutOfBounds, so a caller already checking
   * Buffer::GetAndClearLastError once per packet does not have to check this
   * separately.
   */
  ZNET_NODISCARD bool ok() const { return ok_; }

  /** @brief Bits consumed so far. */
  ZNET_NODISCARD size_t bits_read() const { return bits_read_; }

  /** @brief Bits still available, counting whole bytes left in the buffer. */
  ZNET_NODISCARD size_t remaining_bits() const {
    return buffer_.readable_bytes() * 8u + scratch_bits_;
  }

 private:
  /** @brief Pulls one more byte out of the buffer. */
  bool Refill() {
    if (ZNET_UNLIKELY(buffer_.readable_bytes() == 0)) ZNET_UNLIKELY_ATTR {
      // attempt it anyway. The read fails and does not move the cursor, but it
      // records ReadOutOfBounds on the buffer, which keeps a bit stream's
      // failures on the same channel as every other read in the packet.
      static_cast<void>(buffer_.ReadInt<uint8_t>());
      ok_ = false;
      return false;
    }
    scratch_ |= static_cast<uint64_t>(buffer_.ReadInt<uint8_t>())
                << scratch_bits_;
    scratch_bits_ += 8;
    return true;
  }

  Buffer& buffer_;
  uint64_t scratch_ = 0;
  unsigned scratch_bits_ = 0;
  size_t bits_read_ = 0;
  bool ok_ = true;
};

}  // namespace ext
}  // namespace znet
