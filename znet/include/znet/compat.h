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
// Standard-version detection and the shims that let znet build as C++14 while
// still using the C++17/20 spelling of a feature when the compiler offers it.
//
// The rule for everything in here: the C++20 path must expand to exactly what
// the code used before this header existed, and the fallback must generate the
// same machine code, not merely the same behavior. Nothing here may cost a
// branch, an allocation, or an indirection that the C++20 path does not pay.
//

#ifndef ZNET_COMPAT_H_
#define ZNET_COMPAT_H_

#include <type_traits>

// ---------------------------------------------------------------------------
// Language level
// ---------------------------------------------------------------------------
// MSVC reports 199711L in __cplusplus unless /Zc:__cplusplus is passed, but it
// always reports the real level in _MSVC_LANG. Prefer the latter so znet does
// not silently fall back to the C++14 paths on a C++20 MSVC build.
#if defined(_MSVC_LANG)
#define ZNET_CPLUSPLUS _MSVC_LANG
#else
#define ZNET_CPLUSPLUS __cplusplus
#endif

#define ZNET_HAS_CXX14 (ZNET_CPLUSPLUS >= 201402L)
#define ZNET_HAS_CXX17 (ZNET_CPLUSPLUS >= 201703L)
#define ZNET_HAS_CXX20 (ZNET_CPLUSPLUS >= 202002L)

#if !ZNET_HAS_CXX14
#error "znet requires C++14 or newer."
#endif

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------
#if ZNET_HAS_CXX17
#define ZNET_NODISCARD [[nodiscard]]
#define ZNET_MAYBE_UNUSED [[maybe_unused]]
#define ZNET_FALLTHROUGH [[fallthrough]]
#else
#define ZNET_NODISCARD
#define ZNET_MAYBE_UNUSED
#define ZNET_FALLTHROUGH
#endif

// branch hints. GCC and Clang expose __builtin_expect in every language mode
// and it lowers identically to [[likely]]/[[unlikely]], so the attribute is
// only worth emitting on MSVC, which has no builtin. Splitting the hint into a
// condition wrapper and a statement attribute is what lets a single call site
// get the best available hint on every compiler/standard combination:
//
//   if (ZNET_UNLIKELY(x)) ZNET_UNLIKELY_ATTR { ... }
//
#if defined(__GNUC__) || defined(__clang__)
#define ZNET_LIKELY(x) (__builtin_expect(!!(x), 1))
#define ZNET_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define ZNET_LIKELY_ATTR
#define ZNET_UNLIKELY_ATTR
#elif ZNET_HAS_CXX20
#define ZNET_LIKELY(x) (x)
#define ZNET_UNLIKELY(x) (x)
#define ZNET_LIKELY_ATTR [[likely]]
#define ZNET_UNLIKELY_ATTR [[unlikely]]
#else
#define ZNET_LIKELY(x) (x)
#define ZNET_UNLIKELY(x) (x)
#define ZNET_LIKELY_ATTR
#define ZNET_UNLIKELY_ATTR
#endif

// ---------------------------------------------------------------------------
// Keywords
// ---------------------------------------------------------------------------
// constinit only asserts that no dynamic initialization happens; dropping it
// pre-C++20 loses the diagnostic, never the semantics.
#if ZNET_HAS_CXX20
#define ZNET_CONSTINIT constinit
#else
#define ZNET_CONSTINIT
#endif

// consteval degrades to constexpr. Every znet use returns a literal computed
// from macros, so it still folds at compile time under any optimizer.
#if ZNET_HAS_CXX20
#define ZNET_CONSTEVAL consteval
#else
#define ZNET_CONSTEVAL constexpr
#endif

// several std::array members (data(), the mutating operator[]) only became
// constexpr in C++17. A function that calls one cannot itself be constexpr in
// a C++14 build; marking it this way keeps the C++17+ guarantee without making
// the C++14 build ill-formed.
#if ZNET_HAS_CXX17
#define ZNET_CONSTEXPR17 constexpr
#else
#define ZNET_CONSTEXPR17
#endif

// A namespace-scope `constexpr` is implicitly const, and therefore internal
// linkage, in C++14. For the value-only constants znet declares in headers
// that is equivalent to an inline variable: every translation unit folds the
// value and nothing takes the address.
#if ZNET_HAS_CXX17
#define ZNET_INLINE_CONSTEXPR inline constexpr
#else
#define ZNET_INLINE_CONSTEXPR constexpr
#endif

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------
// Concepts are kept verbatim on C++20 so diagnostics stay readable there; the
// C++14 fallback is a defaulted non-type template parameter, which does not
// change the signature for callers that pass explicit template arguments
// because it is always last and always defaulted.
#define ZNET_ENABLE_IF(...) \
  typename ::std::enable_if<(__VA_ARGS__), int>::type = 0

#if ZNET_HAS_CXX20
#define ZNET_TPL_CONSTRAINED(Concept, Trait, T) template <Concept T>
#define ZNET_TPL_CONSTRAINED2(Concept, Trait, T, U) template <Concept<U> T>
#else
#define ZNET_TPL_CONSTRAINED(Concept, Trait, T) \
  template <typename T, ZNET_ENABLE_IF(Trait<T>::value)>
#define ZNET_TPL_CONSTRAINED2(Concept, Trait, T, U) \
  template <typename T, ZNET_ENABLE_IF(Trait<T, U>::value)>
#endif

namespace znet {
namespace compat {

/** @brief C++14 stand-in for std::void_t. */
template <typename...>
struct MakeVoid {
  using type = void;
};
template <typename... Ts>
using VoidT = typename MakeVoid<Ts...>::type;

/** @brief C++14 stand-in for std::bool_constant. */
template <bool B>
using BoolConstant = std::integral_constant<bool, B>;

/** @brief C++14 stand-in for std::clamp, restricted to the operator< form
 *  that znet uses. */
template <typename T>
constexpr const T& Clamp(const T& v, const T& lo, const T& hi) {
  return v < lo ? lo : (hi < v ? hi : v);
}

}  // namespace compat
}  // namespace znet

// ---------------------------------------------------------------------------
// Endianness
// ---------------------------------------------------------------------------
// std::endian is C++20. Everything below resolves at preprocessing time, so
// GetSystemEndianness() stays a compile-time constant in every mode.
#if ZNET_HAS_CXX20
#include <bit>
#define ZNET_SYSTEM_IS_BIG_ENDIAN (::std::endian::native == ::std::endian::big)
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#define ZNET_SYSTEM_IS_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#elif defined(_WIN32) || defined(_M_IX86) || defined(_M_X64) || \
    defined(_M_ARM) || defined(_M_ARM64)
#define ZNET_SYSTEM_IS_BIG_ENDIAN false
#elif defined(__BIG_ENDIAN__) || defined(__ARMEB__) || defined(__MIPSEB__) || \
    defined(__s390x__)
#define ZNET_SYSTEM_IS_BIG_ENDIAN true
#elif defined(__LITTLE_ENDIAN__) || defined(__ARMEL__) || defined(__MIPSEL__)
#define ZNET_SYSTEM_IS_BIG_ENDIAN false
#else
#error \
    "znet cannot detect endianness on this target; define ZNET_SYSTEM_IS_BIG_ENDIAN to 0 or 1."
#endif

#endif  // ZNET_COMPAT_H_
