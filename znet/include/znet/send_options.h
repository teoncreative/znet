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
// How one message should be delivered.
//
// Every option is advisory: each transport reads the ones it can honor and
// ignores the rest, silently. Nothing is rejected or logged, so an option a
// transport does not implement is inert rather than an error, and no call site
// on the wrong transport will tell you. Which transports honor a given option
// is documented on that option's own Key below. Today all of them are ZDT-only.
//
// Adding an option: a Key struct with the next free id, a field in
// SendOptionsInit and one in SendOptions::Data, both in id order, a builder,
// and a Get() specialization. Then say on the Key which transports honor it,
// and what the ones that do not do instead, since that differs per option and
// is the part callers get wrong.
//
// Keys rather than plain fields because a transport has to tell "the caller
// asked for this" from "the caller said nothing", and answer the second with
// its own default rather than with a zero. That is also what lets a new
// transport pick a different default for an existing option without changing
// the meaning of any call site that never set it.
//

#ifndef ZNET_PARENT_SEND_OPTIONS_H
#define ZNET_PARENT_SEND_OPTIONS_H

#include "znet/precompiled.h"
#include <cstdint>
#include <type_traits>

/**
 * @brief Retransmit until acknowledged. Default true.
 *
 * **Transports:** ZDT only.
 *
 * TCP ignores it and is always reliable, so `false` is not merely unsupported
 * there, it is inexpressible: there is no way to ask kernel TCP to drop a
 * message. Code that sets this and then runs on TCP gets reliable delivery and
 * no indication that it asked for anything else.
 */
struct ReliableKey { using type = bool; static constexpr int id = 0; };
/**
 * @brief Deliver in send order. Default true.
 *
 * **Transports:** ZDT only.
 *
 * Without ReliableKey this means *sequenced* rather than ordered: a message
 * older than one already delivered is dropped instead of held, since waiting
 * for something that will never be retransmitted would stall the channel.
 *
 * TCP ignores it and is always ordered, for the same reason ReliableKey gives.
 */
struct OrderedKey  { using type = bool; static constexpr int id = 1; };
/**
 * @brief Which of the 256 channels to send on. Default 0.
 *
 * **Transports:** ZDT only.
 *
 * Channels have independent sequence spaces, so ordered traffic on one never
 * waits behind another. Allocated lazily, so unused ones cost nothing.
 *
 * TCP has one stream and ignores it, which collapses traffic you had
 * deliberately separated back onto a single ordered pipe. That is the one case
 * where ignoring an option changes throughput rather than just semantics: a
 * bulk transfer and chat share a channel again, and head-of-line block each
 * other.
 */
struct ChannelKey  { using type = uint8_t; static constexpr int id = 2; };

/**
 * @brief Plain-field mirror of SendOptions, for designated initializers.
 *
 * Only useful as the argument to SendOptions' converting constructor. The
 * builder on SendOptions itself says the same thing in every language mode,
 * so reach for this only if you specifically want the designated-initializer
 * spelling on a C++20 build.
 *
 * One field per Key, in id order. A new option adds a field here too, and the
 * order must keep matching, since designated initializers require it.
 */
struct SendOptionsInit {
  bool reliable = true;
  bool ordered = true;
  uint8_t channel = 0;
};

/**
 * @brief Per-message delivery options: reliability, ordering and channel.
 *
 * Passed to PeerSession::SendPacket(). **Every option is currently ZDT-only**,
 * as documented on each Key. On a TCP session the whole object is ignored,
 * silently.
 *
 * An option left unset is not the same as one set to its default value: the
 * transport supplies its own default for anything unset, which for ZDT is
 * reliable, ordered, channel 0.
 *
 * Build the handful your application needs once, as constants, and pass those
 * to every send. Every builder is constexpr, so a namespace-scope constant is
 * constant-initialized rather than built at each call site:
 * @code
 * constexpr SendOptions kPosition = SendOptions().Reliable(false).Channel(1);
 * session->SendPacket(packet, kPosition);
 * @endcode
 */
class SendOptions {
 public:
  /** @brief Sets nothing, so every option is the transport's default. */
  constexpr SendOptions() = default;

  /**
   * @brief Sets all three options at once from a SendOptionsInit.
   *
   * Written with designated initializers, which are **C++20**:
   * @code
   * SendOptions opts{{.reliable = false, .channel = 2}};
   * @endcode
   * Note the double brace: the inner one builds the SendOptionsInit. GCC and
   * Clang accept the syntax below C++20 as an extension, but not under
   * -pedantic, and MSVC does not. The builder below compiles everywhere and is
   * the recommended spelling; this constructor stays for code that prefers
   * designators on a C++20 build.
   *
   * Unlike the default constructor this marks all three as set, so none of them
   * falls back to the transport's default.
   */
  constexpr explicit SendOptions(const SendOptionsInit& init)
      : bitmask_((1u << ReliableKey::id) | (1u << OrderedKey::id) |
                 (1u << ChannelKey::id)),
        data_{init.reliable, init.ordered, init.channel} {}

  /**
   * @brief Returns a copy with reliability set, marking it explicitly chosen.
   *
   * @code
   * constexpr SendOptions kVoice = SendOptions().Reliable(false);
   * @endcode
   */
  ZNET_NODISCARD constexpr SendOptions Reliable(bool value) const {
    return SendOptions(bitmask_ | (1u << ReliableKey::id),
                       Data{value, data_.ordered, data_.channel});
  }

  /** @brief Returns a copy with ordering set, marking it explicitly chosen. */
  ZNET_NODISCARD constexpr SendOptions Ordered(bool value) const {
    return SendOptions(bitmask_ | (1u << OrderedKey::id),
                       Data{data_.reliable, value, data_.channel});
  }

  /** @brief Returns a copy with the channel set, marking it explicitly chosen. */
  ZNET_NODISCARD constexpr SendOptions Channel(uint8_t value) const {
    return SendOptions(bitmask_ | (1u << ChannelKey::id),
                       Data{data_.reliable, data_.ordered, value});
  }

  /**
   * @brief Sets one option in place, marking it as explicitly chosen.
   *
   * The mutating counterpart of the builders above, for the rare case where an
   * option is decided at runtime rather than baked into a constant.
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
  constexpr typename Key::type GetOr(typename Key::type def) const {
    return Has<Key>() ? Get<Key>() : def;
  }

  /** @brief Whether this option was explicitly set. */
  template <typename Key>
  constexpr bool Has() const {
    return (bitmask_ & (1u << Key::id)) != 0;
  }

 private:
  struct Data {
    bool reliable = true;
    bool ordered = true;
    uint8_t channel = 0;
  };

  constexpr SendOptions(uint32_t bitmask, const Data& data)
      : bitmask_(bitmask), data_(data) {}

  uint32_t bitmask_ = 0;
  Data data_;

  template <typename Key>
  typename Key::type& Get() {
    return const_cast<typename Key::type&>(
        static_cast<const SendOptions*>(this)->Get<Key>());
  }

  template <typename Key>
  constexpr const typename Key::type& Get() const {
    static_assert(sizeof(Key) == 0, "Unsupported key");
  }
};

template <> constexpr const bool& SendOptions::Get<ReliableKey>() const { return data_.reliable; }
template <> constexpr const bool& SendOptions::Get<OrderedKey>()  const { return data_.ordered;  }
template <> constexpr const uint8_t& SendOptions::Get<ChannelKey>() const { return data_.channel; }

#endif  //ZNET_PARENT_SEND_OPTIONS_H
