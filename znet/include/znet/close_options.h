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
// How one close should be performed. Keyed like SendOptions, and for the same
// reason: a transport has to tell an explicit choice from an absent one.
//

#ifndef ZNET_CLOSE_OPTIONS_H_
#define ZNET_CLOSE_OPTIONS_H_

#include "znet/precompiled.h"
#include <cstdint>
#include <type_traits>

namespace znet {

/**
 * @brief Per-call options for PeerSession::Close().
 *
 * Default-constructed sets nothing, leaving every choice to the transport.
 */
class CloseOptions {
 public:
  /**
   * @brief Sets one option, marking it as explicitly chosen.
   *
   * @tparam Key NoLingerKey.
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
    bool no_linger = true;
  } data_;

  template <typename Key>
  typename Key::type& Get() {
    return const_cast<typename Key::type&>(
        static_cast<const CloseOptions*>(this)->Get<Key>());
  }

  template <typename Key>
  const typename Key::type& Get() const {
    static_assert(sizeof(Key) == 0, "Unsupported key");
  }
};

/**
 * @brief Discard anything still queued and close at once (SO_LINGER, 0).
 *
 * TCP only. The peer sees a reset rather than a clean shutdown, so use it to
 * drop a connection that is misbehaving, not to end a healthy one.
 */
struct NoLingerKey { using type = bool; static constexpr int id = 0; };

template <> inline const bool& CloseOptions::Get<NoLingerKey>() const { return data_.no_linger; }

}  // namespace znet


#endif  // ZNET_CLOSE_OPTIONS_H_
