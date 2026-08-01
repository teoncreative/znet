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
// how a component crosses the wire.
//
// the archive cannot know how to serialize a game's own component types, so
// this is a customization point: define
//
//     void SerializeComponent(znet::Buffer&, const YourComponent&);
//     void DeserializeComponent(znet::Buffer&, YourComponent&);
//
// in the same namespace as the component, and argument-dependent lookup finds
// it. Arithmetic types, enums and std::string already have defaults; anything
// else without an overload is a compile error naming the type, rather than a
// silent memcpy of whatever padding the struct happens to contain.
//

#pragma once

#include <string>
#include <type_traits>

#include "znet/buffer.h"

namespace znet {
namespace ext {

namespace detail {

/** @brief A false that only becomes false once T is known, so a static_assert
 *  in an uninstantiated template stays quiet. */
template <typename>
struct AlwaysFalse : std::false_type {};

}  // namespace detail

// --- arithmetic -------------------------------------------------------------

template <typename T>
typename std::enable_if<std::is_arithmetic<T>::value>::type SerializeComponent(
    Buffer& buffer, const T& value) {
  buffer.WriteNumber(value);
}

template <typename T>
typename std::enable_if<std::is_arithmetic<T>::value>::type
DeserializeComponent(Buffer& buffer, T& value) {
  value = buffer.ReadNumber<T>();
}

// --- bool -------------------------------------------------------------------

// not left to the arithmetic path: ReadNumber memcpys the raw byte, and a bool
// holding anything but 0 or 1 is undefined, so both `b` and `!b` can test true.
// a peer can trivially send such a byte.
inline void SerializeComponent(Buffer& buffer, const bool& value) {
  buffer.WriteInt<uint8_t>(static_cast<uint8_t>(value ? 1 : 0));
}

inline void DeserializeComponent(Buffer& buffer, bool& value) {
  value = buffer.ReadInt<uint8_t>() != 0;
}

// --- enums ------------------------------------------------------------------

template <typename T>
typename std::enable_if<std::is_enum<T>::value>::type SerializeComponent(
    Buffer& buffer, const T& value) {
  buffer.WriteNumber(
      static_cast<typename std::underlying_type<T>::type>(value));
}

template <typename T>
typename std::enable_if<std::is_enum<T>::value>::type DeserializeComponent(
    Buffer& buffer, T& value) {
  value = static_cast<T>(
      buffer.ReadNumber<typename std::underlying_type<T>::type>());
}

// --- strings ----------------------------------------------------------------

inline void SerializeComponent(Buffer& buffer, const std::string& value) {
  buffer.WriteString(value);
}

inline void DeserializeComponent(Buffer& buffer, std::string& value) {
  value = buffer.ReadString();
}

// --- everything else --------------------------------------------------------

template <typename T>
typename std::enable_if<!std::is_arithmetic<T>::value &&
                        !std::is_enum<T>::value>::type
SerializeComponent(Buffer&, const T&) {
  static_assert(detail::AlwaysFalse<T>::value,
                "This component has no SerializeComponent overload. Define "
                "void SerializeComponent(znet::Buffer&, const T&) in the same "
                "namespace as the component so ADL can find it.");
}

template <typename T>
typename std::enable_if<!std::is_arithmetic<T>::value &&
                        !std::is_enum<T>::value>::type
DeserializeComponent(Buffer&, T&) {
  static_assert(detail::AlwaysFalse<T>::value,
                "This component has no DeserializeComponent overload. Define "
                "void DeserializeComponent(znet::Buffer&, T&) in the same "
                "namespace as the component so ADL can find it.");
}

}  // namespace ext
}  // namespace znet
