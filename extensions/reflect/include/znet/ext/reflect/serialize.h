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
// serialization derived from a type's fields, declared or deduced.
//
// dispatch is one `if constexpr` chain rather than a pile of SFINAE overloads,
// which is most of why this extension asks for C++17. The order of the arms is
// the precedence: an explicit ZNET_REFLECT declaration always wins over the
// automatic aggregate walk, so a type can opt out of deduction by declaring
// itself.
//

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "znet/buffer.h"
#include "znet/compat.h"
#include "znet/ext/reflect/aggregate.h"
#include "znet/ext/reflect/reflect.h"

namespace znet {
namespace ext {

/** @brief Bounds applied when reading a value off the wire. */
struct ReflectLimits {
  /**
   * @brief Largest element count accepted for any one container or string.
   *
   * Checked against the bytes actually present as well, so this is a ceiling
   * rather than the only guard.
   */
  size_t max_elements = 1u << 20;
};

namespace detail {

template <typename>
inline constexpr bool kAlwaysFalse = false;

// --- trait helpers ----------------------------------------------------------

template <typename T>
struct IsStdVector : std::false_type {};
template <typename T, typename A>
struct IsStdVector<std::vector<T, A>> : std::true_type {};

template <typename T>
struct IsStdArray : std::false_type {};
template <typename T, size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type {};

template <typename T>
struct IsStdMap : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct IsStdMap<std::map<K, V, C, A>> : std::true_type {};
template <typename K, typename V, typename H, typename E, typename A>
struct IsStdMap<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template <typename T>
void WriteValue(Buffer& buffer, const T& value);
template <typename T>
bool ReadValue(Buffer& buffer, T& value, const ReflectLimits& limits);

/**
 * @brief Reads a length, refusing one the packet cannot back.
 *
 * The count is attacker-controlled and is about to size an allocation and
 * bound a loop. Every element costs at least one byte on the wire whatever its
 * type, so a count above readable_bytes cannot be honest; that bound plus the
 * configured ceiling is what stops a nine-byte packet asking for a gigabyte.
 */
inline bool ReadCount(Buffer& buffer, const ReflectLimits& limits,
                      size_t& out) {
  const size_t count = buffer.ReadVarInt<size_t>();
  if (count > limits.max_elements || count > buffer.readable_bytes()) {
    return false;
  }
  out = count;
  return true;
}

/** @brief Writes each field the walk hands over. */
struct WriteVisitor {
  Buffer& buffer;

  // the aggregate walker passes values alone; the macro walker passes a name
  // too, which binary output has no use for
  template <typename T>
  void operator()(const T& value) {
    WriteValue(buffer, value);
  }
  template <typename T>
  void operator()(const char*, const T& value) {
    WriteValue(buffer, value);
  }
};

/**
 * @brief Reads each field, and stops caring after the first failure.
 *
 * A generated walk cannot break out early, so a failed field latches a flag
 * and the rest are skipped rather than read out of a buffer already known to
 * be wrong.
 */
struct ReadVisitor {
  Buffer& buffer;
  const ReflectLimits& limits;
  bool ok = true;

  template <typename T>
  void operator()(T& value) {
    if (ok) {
      ok = ReadValue(buffer, value, limits);
    }
  }
  template <typename T>
  void operator()(const char*, T& value) {
    if (ok) {
      ok = ReadValue(buffer, value, limits);
    }
  }
};

template <typename T>
void WriteValue(Buffer& buffer, const T& value) {
  using Clean = std::remove_cv_t<T>;

  if constexpr (std::is_same_v<Clean, bool>) {
    // not the arithmetic arm: ReadNumber memcpys the raw byte, and a bool
    // holding anything but 0 or 1 is undefined, so both `b` and `!b` can test
    // true. A peer can trivially send such a byte.
    buffer.WriteInt<uint8_t>(static_cast<uint8_t>(value ? 1 : 0));
  } else if constexpr (std::is_arithmetic_v<Clean>) {
    buffer.WriteNumber(value);
  } else if constexpr (std::is_enum_v<Clean>) {
    buffer.WriteNumber(static_cast<std::underlying_type_t<Clean>>(value));
  } else if constexpr (std::is_same_v<Clean, std::string>) {
    buffer.WriteString(value);
  } else if constexpr (IsStdArray<Clean>::value) {
    // no count: the arity is part of the type on both ends
    for (const auto& element : value) {
      WriteValue(buffer, element);
    }
  } else if constexpr (IsStdVector<Clean>::value) {
    buffer.WriteVarInt(value.size());
    for (const auto& element : value) {
      WriteValue(buffer, element);
    }
  } else if constexpr (IsStdMap<Clean>::value) {
    buffer.WriteVarInt(value.size());
    for (const auto& entry : value) {
      WriteValue(buffer, entry.first);
      WriteValue(buffer, entry.second);
    }
  } else if constexpr (IsReflected<Clean>::value) {
    WriteVisitor visitor{buffer};
    VisitFields(value, visitor);
  } else if constexpr (IsWalkableAggregate<Clean>::value) {
    WriteVisitor visitor{buffer};
    VisitAggregate(value, visitor);
  } else {
    static_assert(kAlwaysFalse<T>,
                  "This type is not an aggregate and has no ZNET_REFLECT "
                  "declaration, and is not one of the built-in categories. "
                  "Declare it with ZNET_REFLECT, or give it WriteValue and "
                  "ReadValue overloads.");
  }
}

template <typename T>
bool ReadValue(Buffer& buffer, T& value, const ReflectLimits& limits) {
  using Clean = std::remove_cv_t<T>;

  if constexpr (std::is_same_v<Clean, bool>) {
    value = buffer.ReadInt<uint8_t>() != 0;
    return true;
  } else if constexpr (std::is_arithmetic_v<Clean>) {
    value = buffer.ReadNumber<Clean>();
    return true;
  } else if constexpr (std::is_enum_v<Clean>) {
    value = static_cast<Clean>(
        buffer.ReadNumber<std::underlying_type_t<Clean>>());
    return true;
  } else if constexpr (std::is_same_v<Clean, std::string>) {
    size_t size = 0;
    if (!ReadCount(buffer, limits, size)) {
      return false;
    }
    value.assign(buffer.read_cursor_data(), size);
    buffer.SkipRead(size);
    return true;
  } else if constexpr (IsStdArray<Clean>::value) {
    for (auto& element : value) {
      if (!ReadValue(buffer, element, limits)) {
        return false;
      }
    }
    return true;
  } else if constexpr (IsStdVector<Clean>::value) {
    size_t count = 0;
    if (!ReadCount(buffer, limits, count)) {
      return false;
    }
    value.clear();
    value.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      typename Clean::value_type element{};
      if (!ReadValue(buffer, element, limits)) {
        return false;
      }
      value.push_back(std::move(element));
    }
    return true;
  } else if constexpr (IsStdMap<Clean>::value) {
    size_t count = 0;
    if (!ReadCount(buffer, limits, count)) {
      return false;
    }
    value.clear();
    for (size_t i = 0; i < count; ++i) {
      typename Clean::key_type key{};
      typename Clean::mapped_type mapped{};
      if (!ReadValue(buffer, key, limits) ||
          !ReadValue(buffer, mapped, limits)) {
        return false;
      }
      value.emplace(std::move(key), std::move(mapped));
    }
    return true;
  } else if constexpr (IsReflected<Clean>::value) {
    ReadVisitor visitor{buffer, limits};
    VisitFields(value, visitor);
    return visitor.ok;
  } else if constexpr (IsWalkableAggregate<Clean>::value) {
    ReadVisitor visitor{buffer, limits};
    VisitAggregate(value, visitor);
    return visitor.ok;
  } else {
    static_assert(kAlwaysFalse<T>,
                  "This type is not an aggregate and has no ZNET_REFLECT "
                  "declaration, and is not one of the built-in categories.");
    return false;
  }
}

}  // namespace detail

/** @brief Whether this extension can serialize @p T without help. */
template <typename T>
struct IsSerializable
    : std::integral_constant<bool,
                             IsReflected<std::remove_cv_t<T>>::value ||
                                 detail::IsWalkableAggregate<
                                     std::remove_cv_t<T>>::value> {};

/**
 * @brief Writes every field of @p value, in declaration order.
 *
 * @code
 *   struct Player { uint32_t id; std::string name; std::vector<int> items; };
 *   znet::ext::WriteAuto(*buffer, player);   // nothing declared
 * @endcode
 */
template <typename T>
void WriteAuto(Buffer& buffer, const T& value) {
  detail::WriteValue(buffer, value);
}

/**
 * @brief Reads a value written by WriteAuto.
 *
 * @return false when the packet was truncated or a length was implausible.
 *         The destination is left partly written in that case, so decode into
 *         a scratch object when that matters.
 */
template <typename T>
bool ReadAuto(Buffer& buffer, T& value,
              const ReflectLimits& limits = ReflectLimits()) {
  if (!detail::ReadValue(buffer, value, limits)) {
    return false;
  }
  // a field can run off the end without any length looking wrong, so the
  // buffer's own verdict is the last word.
  return buffer.GetAndClearLastError() == BufferError::None;
}

/** @brief The number of fields WriteAuto will visit. */
template <typename T>
constexpr size_t FieldCountOf() {
  if constexpr (IsReflected<std::remove_cv_t<T>>::value) {
    return Reflect<std::remove_cv_t<T>>::field_count;
  } else {
    return detail::FieldCount<T>();
  }
}

}  // namespace ext
}  // namespace znet
