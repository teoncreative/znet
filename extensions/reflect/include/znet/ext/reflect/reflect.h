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
// declaring a type's fields by hand.
//
// the automatic walk in aggregate.h covers plain structs and needs nothing
// declared. This is the escape hatch for everything else: a type with a
// constructor, private members, a base class, or more fields than deduction
// handles. One line naming the members and the same walk works.
//
//   struct Player { uint32_t id; std::string name; Player(); };
//   ZNET_REFLECT(Player, id, name)
//
// it also recovers field *names*, which deduction cannot before C++26, so a
// debug dump or a diff can use the same declaration.
//

#pragma once

#include <cstddef>
#include <type_traits>

namespace znet {
namespace ext {

/**
 * @brief Specialised by ZNET_REFLECT to describe a type's fields.
 *
 * The primary template is the "not declared" case, which is what the
 * serializer keys off when it decides between this and deduction.
 */
template <typename T>
struct Reflect {
  static constexpr bool value = false;
  static constexpr size_t field_count = 0;
};

/** @brief Whether ZNET_REFLECT has been used on @p T. */
template <typename T>
struct IsReflected : std::integral_constant<bool, Reflect<T>::value> {};

/**
 * @brief Calls @p visitor with (name, member) for each declared field.
 *
 * Works on const and non-const objects; the visitor gets a matching reference,
 * so one walk both reads and writes.
 */
template <typename T, typename Visitor>
void VisitFields(T& value, Visitor& visitor) {
  Reflect<std::remove_cv_t<T>>::Visit(value, visitor);
}

}  // namespace ext
}  // namespace znet

// ---------------------------------------------------------------------------
// Preprocessor plumbing
// ---------------------------------------------------------------------------
//
// a variadic field list has to be walked by the preprocessor, which cannot
// loop, so the expansion is written out once per arity. MSVC needs
// /Zc:preprocessor for __VA_ARGS__ to behave; znet already sets it.

#define ZNET_RF_EXPAND(x) x
// `object`, not `value`: the trait has a static member called value, and a
// parameter of that name would shadow it in every consumer built with
// -Wshadow. A warning coming out of a library macro is the user's problem
// to silence and they cannot.
#define ZNET_RF_VISIT_ONE(field) visitor(#field, object.field);
#define ZNET_RF_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, N, ...) N
#define ZNET_RF_NARG(...) \
    ZNET_RF_EXPAND(ZNET_RF_ARG_N(__VA_ARGS__, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))

#define ZNET_RF_FE_1(f, x) f(x)
#define ZNET_RF_FE_2(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_1(f, __VA_ARGS__))
#define ZNET_RF_FE_3(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_2(f, __VA_ARGS__))
#define ZNET_RF_FE_4(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_3(f, __VA_ARGS__))
#define ZNET_RF_FE_5(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_4(f, __VA_ARGS__))
#define ZNET_RF_FE_6(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_5(f, __VA_ARGS__))
#define ZNET_RF_FE_7(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_6(f, __VA_ARGS__))
#define ZNET_RF_FE_8(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_7(f, __VA_ARGS__))
#define ZNET_RF_FE_9(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_8(f, __VA_ARGS__))
#define ZNET_RF_FE_10(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_9(f, __VA_ARGS__))
#define ZNET_RF_FE_11(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_10(f, __VA_ARGS__))
#define ZNET_RF_FE_12(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_11(f, __VA_ARGS__))
#define ZNET_RF_FE_13(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_12(f, __VA_ARGS__))
#define ZNET_RF_FE_14(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_13(f, __VA_ARGS__))
#define ZNET_RF_FE_15(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_14(f, __VA_ARGS__))
#define ZNET_RF_FE_16(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_15(f, __VA_ARGS__))
#define ZNET_RF_FE_17(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_16(f, __VA_ARGS__))
#define ZNET_RF_FE_18(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_17(f, __VA_ARGS__))
#define ZNET_RF_FE_19(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_18(f, __VA_ARGS__))
#define ZNET_RF_FE_20(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_19(f, __VA_ARGS__))
#define ZNET_RF_FE_21(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_20(f, __VA_ARGS__))
#define ZNET_RF_FE_22(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_21(f, __VA_ARGS__))
#define ZNET_RF_FE_23(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_22(f, __VA_ARGS__))
#define ZNET_RF_FE_24(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_23(f, __VA_ARGS__))
#define ZNET_RF_FE_25(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_24(f, __VA_ARGS__))
#define ZNET_RF_FE_26(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_25(f, __VA_ARGS__))
#define ZNET_RF_FE_27(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_26(f, __VA_ARGS__))
#define ZNET_RF_FE_28(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_27(f, __VA_ARGS__))
#define ZNET_RF_FE_29(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_28(f, __VA_ARGS__))
#define ZNET_RF_FE_30(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_29(f, __VA_ARGS__))
#define ZNET_RF_FE_31(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_30(f, __VA_ARGS__))
#define ZNET_RF_FE_32(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_31(f, __VA_ARGS__))
#define ZNET_RF_FE_33(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_32(f, __VA_ARGS__))
#define ZNET_RF_FE_34(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_33(f, __VA_ARGS__))
#define ZNET_RF_FE_35(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_34(f, __VA_ARGS__))
#define ZNET_RF_FE_36(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_35(f, __VA_ARGS__))
#define ZNET_RF_FE_37(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_36(f, __VA_ARGS__))
#define ZNET_RF_FE_38(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_37(f, __VA_ARGS__))
#define ZNET_RF_FE_39(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_38(f, __VA_ARGS__))
#define ZNET_RF_FE_40(f, x, ...) \
    f(x) ZNET_RF_EXPAND(ZNET_RF_FE_39(f, __VA_ARGS__))

#define ZNET_RF_CONCAT_(a, b) a##b
#define ZNET_RF_CONCAT(a, b) ZNET_RF_CONCAT_(a, b)
#define ZNET_RF_FOR_EACH(f, ...) \
  ZNET_RF_EXPAND(ZNET_RF_CONCAT(ZNET_RF_FE_, ZNET_RF_NARG(__VA_ARGS__))(f, __VA_ARGS__))

/**
 * @brief Declares the serializable fields of @p Type.
 *
 * Use it at global scope, naming the type in full:
 *
 *   namespace game { struct Player { uint32_t id; std::string name; }; }
 *   ZNET_REFLECT(game::Player, id, name)
 *
 * Field order is the wire order, so reordering the arguments changes the
 * format even though the struct did not. A declaration takes precedence over
 * the automatic walk, which is how an aggregate opts out of deduction, for
 * instance to leave a field off the wire. Up to 40 fields.
 */
#define ZNET_REFLECT(Type, ...)                                              \
  namespace znet {                                                           \
  namespace ext {                                                            \
  template <>                                                                \
  struct Reflect<Type> {                                                     \
    static constexpr bool value = true;                                      \
    static constexpr size_t field_count = ZNET_RF_NARG(__VA_ARGS__);         \
    static const char* name() { return #Type; }                              \
    template <typename Visitor>                                              \
    static void Visit(Type& object, Visitor& visitor) {                       \
      ZNET_RF_FOR_EACH(ZNET_RF_VISIT_ONE, __VA_ARGS__)                       \
    }                                                                        \
    template <typename Visitor>                                              \
    static void Visit(const Type& object, Visitor& visitor) {                 \
      ZNET_RF_FOR_EACH(ZNET_RF_VISIT_ONE, __VA_ARGS__)                       \
    }                                                                        \
  };                                                                         \
  }                                                                          \
  }
