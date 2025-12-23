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
// Created by Metehan Gezer on 07/08/2025.
//

#ifndef ZNET_PARENT_SEND_OPTIONS_H
#define ZNET_PARENT_SEND_OPTIONS_H

#include "znet/precompiled.h"
#include <cstdint>
#include <type_traits>

class SendOptions {
 public:
  template <typename Key>
  void Set(typename Key::type value) {
    bitmask_ |= (1u << Key::id);
    Get<Key>() = value;
  }

  template <typename Key>
  typename Key::type GetOr(typename Key::type def) const {
    return Has<Key>() ? Get<Key>() : def;
  }

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

struct ReliableKey { using type = bool; static constexpr int id = 0; };
struct OrderedKey  { using type = bool; static constexpr int id = 1; };
struct ChannelKey  { using type = uint8_t; static constexpr int id = 2; };

// specializations
template <> inline const bool& SendOptions::Get<ReliableKey>() const { return data_.reliable; }
template <> inline const bool& SendOptions::Get<OrderedKey>()  const { return data_.ordered;  }
template <> inline const uint8_t& SendOptions::Get<ChannelKey>() const { return data_.channel; }

#endif  //ZNET_PARENT_SEND_OPTIONS_H
