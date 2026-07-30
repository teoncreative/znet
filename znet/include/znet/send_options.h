//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// How one message should be delivered. Only ZDT reads these; TCP is a single
// reliable ordered stream and ignores them.
//
// Keys rather than plain fields because a transport has to tell "the caller
// asked for this" from "the caller said nothing", and answer the second with
// its own default rather than with a zero.
//

#ifndef ZNET_PARENT_SEND_OPTIONS_H
#define ZNET_PARENT_SEND_OPTIONS_H

#include "znet/precompiled.h"
#include <cstdint>
#include <type_traits>

struct ReliableKey;
struct OrderedKey;
struct ChannelKey;

/**
 * @brief Plain-field mirror of SendOptions, for designated initializers.
 *
 * Only useful as the argument to SendOptions' converting constructor.
 */
struct SendOptionsInit {
  bool reliable = true;
  bool ordered = true;
  uint8_t channel = 0;
};

/**
 * @brief Per-message delivery options: reliability, ordering and channel.
 *
 * Passed to PeerSession::SendPacket(). An option left unset is not the same as
 * one set to its default value: the transport supplies its own default for
 * anything unset, which for ZDT is reliable, ordered, channel 0.
 */
class SendOptions {
 public:
  /** @brief Sets nothing, so every option is the transport's default. */
  SendOptions() = default;

  /**
   * @brief Sets all three options at once from a SendOptionsInit.
   *
   * Written with designated initializers, which are **C++20**:
   * @code
   * SendOptions opts{{.reliable = false, .channel = 2}};
   * @endcode
   * Note the double brace: the inner one builds the SendOptionsInit. At C++14
   * and C++17 use Set() instead. GCC and Clang accept the syntax below C++20 as
   * an extension, but not under -pedantic, and MSVC does not.
   *
   * Unlike the default constructor this marks all three as set, so none of them
   * falls back to the transport's default.
   */
  explicit SendOptions(const SendOptionsInit& init);

  /**
   * @brief Sets one option, marking it as explicitly chosen.
   *
   * @tparam Key ReliableKey, OrderedKey or ChannelKey.
   * @code
   * SendOptions opts;
   * opts.Set<ReliableKey>(false);
   * @endcode
   */
  template <typename Key>
  void Set(typename Key::type value) {
    bitmask_ |= (1u << Key::id);
    Get<Key>() = value;
  }

  /** @brief The value if it was set, otherwise @p def. What transports call. */
  template <typename Key>
  typename Key::type GetOr(typename Key::type def) const {
    return Has<Key>() ? Get<Key>() : def;
  }

  /** @brief Whether this option was explicitly set. */
  template <typename Key>
  bool Has() const {
    return bitmask_ & (1u << Key::id);
  }

 private:
  uint32_t bitmask_ = 0;
  struct {
    bool reliable = true;
    bool ordered = true;
    uint8_t channel = 0;
  } data_;

  template <typename Key>
  typename Key::type& Get() {
    return const_cast<typename Key::type&>(
        static_cast<const SendOptions*>(this)->Get<Key>());
  }

  template <typename Key>
  const typename Key::type& Get() const {
    static_assert(sizeof(Key) == 0, "Unsupported key");
  }
};

/** @brief Retransmit until acknowledged. Default true. */
struct ReliableKey { using type = bool; static constexpr int id = 0; };
/**
 * @brief Deliver in send order. Default true.
 *
 * Without ReliableKey this means *sequenced* rather than ordered: a message
 * older than one already delivered is dropped instead of held, since waiting
 * for something that will never be retransmitted would stall the channel.
 */
struct OrderedKey  { using type = bool; static constexpr int id = 1; };
/**
 * @brief Which of the 256 channels to send on. Default 0.
 *
 * Channels have independent sequence spaces, so ordered traffic on one never
 * waits behind another. Allocated lazily, so unused ones cost nothing.
 */
struct ChannelKey  { using type = uint8_t; static constexpr int id = 2; };

template <> inline const bool& SendOptions::Get<ReliableKey>() const { return data_.reliable; }
template <> inline const bool& SendOptions::Get<OrderedKey>()  const { return data_.ordered;  }
template <> inline const uint8_t& SendOptions::Get<ChannelKey>() const { return data_.channel; }

// out of line: the key types have to be complete before Set() can be called
inline SendOptions::SendOptions(const SendOptionsInit& init) {
  Set<ReliableKey>(init.reliable);
  Set<OrderedKey>(init.ordered);
  Set<ChannelKey>(init.channel);
}

#endif  //ZNET_PARENT_SEND_OPTIONS_H
