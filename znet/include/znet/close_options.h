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

#ifndef ZNET_PARENT_CLOSE_OPTIONS_H
#define ZNET_PARENT_CLOSE_OPTIONS_H

#include "znet/precompiled.h"
#include <cstdint>
#include <type_traits>

class CloseOptions {
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

struct NoLingerKey { using type = bool; static constexpr int id = 0; };

// specializations
template <> inline const bool& CloseOptions::Get<NoLingerKey>() const { return data_.no_linger; }

#endif  //ZNET_PARENT_CLOSE_OPTIONS_H
