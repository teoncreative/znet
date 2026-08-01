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
#include <limits>

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/ext/bitpack/bits.h"

namespace znet {
namespace ext {

/**
 * @brief Writes sub-byte fields into a Buffer.
 *
 * Everything the core Buffer offers rounds up to a whole byte: a bool costs 8
 * bits, and a value known to be under 1000 costs 16. A tick of game state is
 * mostly such fields, and the rounding is often a third of the packet.
 *
 * Usage: open a writer over the buffer, write fields, let it go out of scope.
 *
 * @code
 *   buffer->WriteInt<uint32_t>(entity_id);   // ordinary buffer field
 *   {
 *     BitWriter bits(*buffer);
 *     bits.WriteBool(packet->firing);              //  1 bit
 *     bits.WriteUIntRanged(packet->ammo, 0, 200);  //  8 bits
 *     bits.WriteIntRanged(packet->health, -50, 100);  // 8 bits
 *     bits.WriteFloatRanged(packet->yaw, -3.15f, 3.15f, 12);  // 12 bits
 *   }                                        // flushes: 4 bytes, not 13
 *   buffer->WriteString(packet->name);       // ordinary buffer field again
 * @endcode
 *
 * The destructor flushes, which is the whole reason to scope it: a hand-rolled
 * bit packer that forgets to write its final partial byte loses the last few
 * fields, and it does so silently and only for some payload lengths. Call
 * Flush() explicitly if you need the byte count before the writer dies.
 *
 * After a flush the buffer is byte-aligned again, so bit fields and ordinary
 * Buffer fields interleave freely in one packet.
 */
class BitWriter {
 public:
  explicit BitWriter(Buffer& buffer) : buffer_(buffer) {}

  ~BitWriter() { Flush(); }

  BitWriter(const BitWriter&) = delete;
  BitWriter& operator=(const BitWriter&) = delete;

  /**
   * @brief Writes the low @p bits bits of @p value, least significant first.
   *
   * @p bits above kMaxBitsPerCall is a programming error, not a wire
   * condition; it is clamped and flagged rather than allowed to shift past the
   * end of the scratch register. Use WriteBits64 for wider fields.
   */
  void WriteBits(uint32_t value, unsigned bits) {
    if (bits == 0) {
      return;
    }
    if (ZNET_UNLIKELY(bits > kMaxBitsPerCall)) ZNET_UNLIKELY_ATTR {
      overflowed_ = true;
      bits = kMaxBitsPerCall;
    }
    const uint32_t masked =
        bits >= 32 ? value : (value & ((UINT32_C(1) << bits) - 1));
    // scratch_bits_ is always below 32 here, so 32 more still fit in 64
    scratch_ |= static_cast<uint64_t>(masked) << scratch_bits_;
    scratch_bits_ += bits;
    bits_written_ += bits;
    if (scratch_bits_ >= 32) {
      const uint8_t bytes[4] = {
          static_cast<uint8_t>(scratch_ & 0xFFu),
          static_cast<uint8_t>((scratch_ >> 8) & 0xFFu),
          static_cast<uint8_t>((scratch_ >> 16) & 0xFFu),
          static_cast<uint8_t>((scratch_ >> 24) & 0xFFu)};
      buffer_.Write(bytes, static_cast<size_t>(4));
      scratch_ >>= 32;
      scratch_bits_ -= 32;
    }
  }

  /** @brief WriteBits for fields wider than 32 bits. */
  void WriteBits64(uint64_t value, unsigned bits) {
    if (bits <= kMaxBitsPerCall) {
      WriteBits(static_cast<uint32_t>(value), bits);
      return;
    }
    if (ZNET_UNLIKELY(bits > 64)) ZNET_UNLIKELY_ATTR {
      overflowed_ = true;
      bits = 64;
    }
    WriteBits(static_cast<uint32_t>(value & 0xFFFFFFFFu), 32);
    WriteBits(static_cast<uint32_t>(value >> 32), bits - 32);
  }

  /** @brief One bit. */
  void WriteBool(bool value) { WriteBits(value ? 1u : 0u, 1); }

  /**
   * @brief Writes @p value in exactly as many bits as [min, max] needs.
   *
   * Values outside the range clamp to the endpoints. A range holding a single
   * value costs nothing at all.
   */
  void WriteUIntRanged(uint64_t value, uint64_t min, uint64_t max) {
    const unsigned bits = BitsForRange(min, max);
    if (bits == 0) {
      return;
    }
    const uint64_t clamped = value < min ? min : (value > max ? max : value);
    WriteBits64(clamped - min, bits);
  }

  /** @brief WriteUIntRanged for a signed range. */
  void WriteIntRanged(int64_t value, int64_t min, int64_t max) {
    const unsigned bits = BitsForSignedRange(min, max);
    if (bits == 0) {
      return;
    }
    const int64_t clamped = value < min ? min : (value > max ? max : value);
    // unsigned subtraction so a range spanning zero cannot overflow
    WriteBits64(
        static_cast<uint64_t>(clamped) - static_cast<uint64_t>(min), bits);
  }

  /**
   * @brief Writes @p value as @p bits of fixed point over [min, max].
   *
   * Worst-case error is (max - min) / (2 * (2^bits - 1)). Twelve bits over a
   * full turn is 0.09 degrees, which is finer than most games interpolate.
   *
   * There is a ceiling worth knowing about: past roughly 21 bits the step
   * becomes smaller than the gap between adjacent float32 values, so the
   * result cannot get any closer to the input and the extra bits are spent for
   * nothing. If a field needs more precision than that, it needs a double or a
   * narrower range, not a wider field.
   */
  void WriteFloatRanged(float value, float min, float max, unsigned bits) {
    if (bits == 0) {
      return;
    }
    if (ZNET_UNLIKELY(bits > kMaxBitsPerCall)) ZNET_UNLIKELY_ATTR {
      overflowed_ = true;
      bits = kMaxBitsPerCall;
    }
    // a degenerate range still spends its bits: the reader's field alignment
    // must not depend on values the sender happened to be holding
    WriteBits(detail::QuantizeFloat(value, min, max, bits), bits);
  }

  /**
   * @brief Writes an unbounded value in 5-bit groups: four data bits and a
   *        continuation bit.
   *
   * For counts and indices with no natural upper bound. Values under 16 cost 5
   * bits, under 256 cost 10, and the worst case is 80. Prefer
   * WriteUIntRanged whenever a bound actually exists -- it is always smaller.
   */
  void WriteVarUInt(uint64_t value) {
    for (;;) {
      const uint32_t nibble = static_cast<uint32_t>(value & 0xFu);
      value >>= 4;
      WriteBits(nibble, 4);
      WriteBool(value != 0);
      if (value == 0) {
        return;
      }
    }
  }

  /** @brief Pads with zero bits up to the next byte boundary. */
  void Align() {
    const unsigned pad = static_cast<unsigned>((8u - (bits_written_ % 8u)) % 8u);
    if (pad != 0) {
      WriteBits(0u, pad);
    }
  }

  /**
   * @brief Writes any pending bits out, zero-padding the final byte.
   *
   * Idempotent, and writing may continue afterwards from a byte boundary. The
   * destructor calls this.
   */
  void Flush() {
    while (scratch_bits_ > 0) {
      buffer_.WriteInt<uint8_t>(static_cast<uint8_t>(scratch_ & 0xFFu));
      scratch_ >>= 8;
      scratch_bits_ = scratch_bits_ >= 8 ? scratch_bits_ - 8 : 0u;
    }
    scratch_ = 0;
    // a partial byte was padded out to a whole one
    bits_written_ = (bits_written_ + 7u) / 8u * 8u;
  }

  /** @brief Bits written so far, including padding from any earlier Flush. */
  ZNET_NODISCARD size_t bits_written() const { return bits_written_; }

  /** @brief Bytes those bits will occupy once flushed. */
  ZNET_NODISCARD size_t bytes_written() const {
    return (bits_written_ + 7u) / 8u;
  }

  /**
   * @brief False once a call asked for more bits than a field can hold.
   *
   * A caller bug rather than a wire condition, kept separate from the buffer's
   * own error so it cannot be mistaken for a malformed packet.
   */
  ZNET_NODISCARD bool ok() const { return !overflowed_; }

 private:
  Buffer& buffer_;
  uint64_t scratch_ = 0;
  unsigned scratch_bits_ = 0;
  size_t bits_written_ = 0;
  bool overflowed_ = false;
};

}  // namespace ext
}  // namespace znet
