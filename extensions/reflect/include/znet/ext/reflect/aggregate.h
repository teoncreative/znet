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
// field access for plain aggregates, with nothing declared.
//
// C++ has no reflection before C++26, but C++17 has enough to fake it for
// aggregates: how many members a type has can be recovered by asking which
// brace-initialiser arities compile, and structured bindings can then name
// them. That covers the shape most packet structs already have.
//
//   struct Player { uint32_t id; std::string name; float health; };
//   // no declaration at all
//
// what it cannot do is recover field *names* -- those need C++26 -- and it
// only works on aggregates: no user-declared constructors, no private members,
// no base classes, no virtuals. ZNET_REFLECT covers everything else.
//

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace znet {
namespace ext {
namespace detail {

/**
 * @brief Converts to anything, so it can stand in for a field of any type.
 *
 * Only ever used unevaluated, inside decltype, which is why it needs no
 * definition. The self-exclusion stops it from satisfying a copy or move
 * constructor and making every type look one-field-constructible.
 */
struct AnyField {
  template <typename T, typename = typename std::enable_if<
                            !std::is_same<T, AnyField>::value>::type>
  constexpr operator T() const noexcept;
};

template <typename T, typename Sequence, typename = void>
struct BraceConstructible : std::false_type {};

template <typename T, size_t... I>
struct BraceConstructible<
    T, std::index_sequence<I...>,
    typename std::enable_if<
        sizeof(decltype(T{(void(I), AnyField{})...})) != 0>::type>
    : std::true_type {};

/**
 * @brief The largest brace-initialiser arity @p T accepts.
 *
 * Counted downwards, so the answer is the widest list that compiles.
 *
 * Brace elision does not inflate the answer for a nested aggregate: AnyField
 * converts to the member's own type, so each member is satisfied from exactly
 * one initialiser and a nested struct counts as the one field it is.
 *
 * Where a type does defeat it, the structured binding below is checked against
 * the real arity by the compiler, so the failure is a build error naming the
 * type rather than a silently wrong wire format. ZNET_REFLECT is the way out.
 */
template <typename T, size_t N>
constexpr size_t FieldCountDown() {
  if constexpr (N == 0) {
    return 0;
  } else if constexpr (BraceConstructible<T, std::make_index_sequence<N>>::value) {
    return N;
  } else {
    return FieldCountDown<T, N - 1>();
  }
}

/** @brief Upper bound on fields the automatic path handles. */
constexpr size_t kMaxAggregateFields = 24;

template <typename T>
constexpr size_t FieldCount() {
  return FieldCountDown<typename std::remove_cv<
      typename std::remove_reference<T>::type>::type,
                        kMaxAggregateFields>();
}

/** @brief Whether @p T can be walked without a ZNET_REFLECT declaration. */
template <typename T>
struct IsWalkableAggregate
    : std::integral_constant<
          bool, std::is_aggregate<typename std::remove_cv<T>::type>::value &&
                    !std::is_array<T>::value && (FieldCount<T>() > 0) &&
                    (FieldCount<T>() <= kMaxAggregateFields)> {};

/**
 * @brief Calls @p visitor once per member of an aggregate.
 *
 * One case per arity, because a structured binding has to name its members
 * literally. Generated rather than written by hand.
 */
template <typename T, typename Visitor>
void VisitAggregate(T& value, Visitor& visitor) {
  constexpr size_t count = FieldCount<T>();
  if constexpr (count == 1) {
    auto& [f0] = value;
    visitor(f0);
  }  else if constexpr (count == 2) {
    auto& [f0, f1] = value;
    visitor(f0); visitor(f1);
  }  else if constexpr (count == 3) {
    auto& [f0, f1, f2] = value;
    visitor(f0); visitor(f1); visitor(f2);
  }  else if constexpr (count == 4) {
    auto& [f0, f1, f2, f3] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3);
  }  else if constexpr (count == 5) {
    auto& [f0, f1, f2, f3, f4] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4);
  }  else if constexpr (count == 6) {
    auto& [f0, f1, f2, f3, f4, f5] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5);
  }  else if constexpr (count == 7) {
    auto& [f0, f1, f2, f3, f4, f5, f6] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6);
  }  else if constexpr (count == 8) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7);
  }  else if constexpr (count == 9) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8);
  }  else if constexpr (count == 10) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9);
  }  else if constexpr (count == 11) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10);
  }  else if constexpr (count == 12) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11);
  }  else if constexpr (count == 13) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12);
  }  else if constexpr (count == 14) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13);
  }  else if constexpr (count == 15) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14);
  }  else if constexpr (count == 16) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15);
  }  else if constexpr (count == 17) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16);
  }  else if constexpr (count == 18) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17);
  }  else if constexpr (count == 19) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17); visitor(f18);
  }  else if constexpr (count == 20) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17); visitor(f18); visitor(f19);
  }  else if constexpr (count == 21) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17); visitor(f18); visitor(f19); visitor(f20);
  }  else if constexpr (count == 22) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17); visitor(f18); visitor(f19); visitor(f20); visitor(f21);
  }  else if constexpr (count == 23) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17); visitor(f18); visitor(f19); visitor(f20); visitor(f21); visitor(f22);
  }  else if constexpr (count == 24) {
    auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23] = value;
    visitor(f0); visitor(f1); visitor(f2); visitor(f3); visitor(f4); visitor(f5); visitor(f6); visitor(f7); visitor(f8); visitor(f9); visitor(f10); visitor(f11); visitor(f12); visitor(f13); visitor(f14); visitor(f15); visitor(f16); visitor(f17); visitor(f18); visitor(f19); visitor(f20); visitor(f21); visitor(f22); visitor(f23);
  }
  else {
    static_assert(count <= kMaxAggregateFields,
                  "This aggregate has more fields than the automatic walker "
                  "handles. Nest a struct, or declare it with ZNET_REFLECT.");
  }
}

}  // namespace detail
}  // namespace ext
}  // namespace znet
