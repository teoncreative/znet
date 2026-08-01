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

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/ext/entt/component.h"

namespace znet {
namespace ext {

/**
 * @brief A Buffer-backed output archive for entt::snapshot.
 *
 * EnTT already knows how to walk a registry and, on the far end, how to remap
 * entity identifiers onto locally allocated ones. What it needs is somewhere
 * to put the bytes. That is all this is, which is deliberate: reimplementing
 * the traversal would mean reimplementing entt::continuous_loader, and getting
 * remapping wrong is the classic way an ECS replication layer corrupts itself.
 *
 * @code
 *   entt::snapshot{registry}
 *       .get<entt::entity>(archive)
 *       .get<Position>(archive)
 *       .get<Health>(archive);
 * @endcode
 *
 * Written for the default 32-bit entt::entity registry.
 */
class EnttOutputArchive {
 public:
  explicit EnttOutputArchive(Buffer& buffer) : buffer_(buffer) {}

  /**
   * @brief An entity identifier, always four bytes.
   *
   * Not a varint: EnTT writes the null entity as all bits set to mark a gap,
   * and that is the worst case for a varint. Fixed width keeps the common gap
   * marker at four bytes instead of five.
   */
  void operator()(::entt::entity value) {
    buffer_.WriteInt<uint32_t>(static_cast<uint32_t>(value));
  }

  /** @brief A length or a free-list count, which are small, so a varint. */
  void operator()(std::uint32_t value) { buffer_.WriteVarInt(value); }

  /** @brief A component, through the SerializeComponent customization point. */
  template <typename T>
  void operator()(const T& value) {
    using znet::ext::SerializeComponent;
    SerializeComponent(buffer_, value);
  }

 private:
  Buffer& buffer_;
};

/**
 * @brief A Buffer-backed input archive for entt::snapshot_loader and
 *        entt::continuous_loader.
 *
 * Counts read here are attacker-controlled and are used directly by EnTT to
 * reserve storage and to bound its own loops, so they are clamped against what
 * the buffer actually holds. Without that, a nine-byte packet claiming four
 * billion entities is both a huge allocation and a four-billion-iteration
 * loop, and EnTT has no way to know better. See operator()(std::uint32_t&).
 */
class EnttInputArchive {
 public:
  explicit EnttInputArchive(Buffer& buffer) : buffer_(buffer) {}

  /**
   * @brief An entity identifier written by EnttOutputArchive.
   *
   * Out of bytes, this yields the null entity rather than a zero, and the
   * difference is not cosmetic. EnTT already has a meaning for null: a
   * component list skips it as a gap, and an entity list asks generate() for a
   * fresh identifier instead. A zero has no such meaning, so a truncated
   * packet hands the same identifier over repeatedly and the loader places one
   * slot twice, which trips an assertion in a debug build and quietly corrupts
   * the sparse set in a release one.
   */
  void operator()(::entt::entity& value) {
    if (buffer_.readable_bytes() < sizeof(uint32_t)) {
      static_cast<void>(buffer_.ReadInt<uint32_t>());  // record it on the buffer
      truncated_ = true;
      value = ::entt::null;
      previous_was_length_ = false;
      return;
    }
    value = static_cast<::entt::entity>(buffer_.ReadInt<uint32_t>());
    previous_was_length_ = false;
  }

  /**
   * @brief A length or count, clamped to what the rest of the packet can hold.
   *
   * Every element EnTT goes on to read costs at least one entity identifier,
   * so a count above readable_bytes/4 cannot be honest. Clamping there turns a
   * malicious count into a short read, which the loader survives, instead of
   * an allocation or a hang.
   */
  void operator()(std::uint32_t& value) {
    const uint32_t claimed = buffer_.ReadVarInt<uint32_t>();

    // every element EnTT goes on to read costs at least one 4-byte entity
    // identifier, so a count above readable_bytes/4 cannot be honest. This is
    // a bound, not an exact budget: EnTT may read a free-list count before it
    // touches any entity, so the loop can still run dry near the end. That is
    // safe because operator()(entt::entity&) yields null once it does.
    size_t ceiling = buffer_.readable_bytes() / sizeof(uint32_t);

    // two lengths in a row, with no entity between them, is the entity storage
    // announcing its size and then its free-list size. A free list cannot be
    // longer than the storage holding it, and EnTT asserts rather than checks:
    // free_list(n) with n past the end is "Invalid value" in a debug build and
    // a corrupt storage in a release one. It is only ever clamped downwards,
    // and a too-small free list costs bookkeeping rather than safety.
    if (previous_was_length_ && static_cast<size_t>(last_length_) < ceiling) {
      ceiling = last_length_;
    }

    if (static_cast<size_t>(claimed) > ceiling) {
      value = static_cast<uint32_t>(ceiling);
      clamped_ = true;
    } else {
      value = claimed;
    }

    last_length_ = value;
    previous_was_length_ = true;
  }

  /** @brief A component, through the DeserializeComponent customization point. */
  template <typename T>
  void operator()(T& value) {
    using znet::ext::DeserializeComponent;
    DeserializeComponent(buffer_, value);
    previous_was_length_ = false;
  }

  /**
   * @brief False if the packet was truncated or claimed more than it carried.
   *
   * Check once after loading. The registry is left holding whatever arrived
   * before the problem, so a caller that cares should load into a scratch
   * registry rather than the live one.
   */
  ZNET_NODISCARD bool ok() const { return !clamped_ && !truncated_; }

  /** @brief Whether a count had to be clamped, which means the packet lied. */
  ZNET_NODISCARD bool clamped() const { return clamped_; }

 private:
  Buffer& buffer_;
  uint32_t last_length_ = 0;
  bool previous_was_length_ = false;
  bool clamped_ = false;
  bool truncated_ = false;
};

}  // namespace ext
}  // namespace znet
