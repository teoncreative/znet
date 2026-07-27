//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/packet.h"
#include "znet/packet_serializer.h"
#include "znet/precompiled.h"

namespace znet {

// packet handler type constraints. as in buffer.h, each is available both as
// a trait (any language mode) and as the original concept (C++20).
namespace detail {

template <typename T, typename P, typename = void>
struct HasOnPacketConstT : std::false_type {};
template <typename T, typename P>
struct HasOnPacketConstT<
    T, P,
    compat::VoidT<decltype(std::declval<T&>().OnPacket(
        std::declval<const P&>()))>>
    : std::is_same<decltype(std::declval<T&>().OnPacket(
                       std::declval<const P&>())),
                   void> {};

template <typename T, typename P, typename = void>
struct HasOnPacketSharedT : std::false_type {};
template <typename T, typename P>
struct HasOnPacketSharedT<
    T, P,
    compat::VoidT<decltype(std::declval<T&>().OnPacket(
        std::declval<std::shared_ptr<P>>()))>>
    : std::is_same<decltype(std::declval<T&>().OnPacket(
                       std::declval<std::shared_ptr<P>>())),
                   void> {};

template <typename T>
struct IsDerivedFromPacket : std::is_base_of<Packet, T> {};

}  // namespace detail

// aliases over the traits above, so each constraint is defined exactly once.
#if ZNET_HAS_CXX20
template<typename T, typename P>
concept HasOnPacketConst = detail::HasOnPacketConstT<T, P>::value;

template<typename T, typename P>
concept HasOnPacketShared = detail::HasOnPacketSharedT<T, P>::value;

template<typename T>
concept DerivedFromPacket = detail::IsDerivedFromPacket<T>::value;
#endif

#define ZNET_TPL_DERIVED_FROM_PACKET(T)                       \
  ZNET_TPL_CONSTRAINED(::znet::DerivedFromPacket,             \
                       ::znet::detail::IsDerivedFromPacket, T)

struct PacketHandlerBase {
  virtual ~PacketHandlerBase() = default;
  virtual void Handle(std::shared_ptr<Packet> p) = 0;
};


// This is a fuming mess, clean it up!
template<typename Derived, typename... PacketTypes>
class PacketHandler : public PacketHandlerBase {
 public:
  void Handle(std::shared_ptr<Packet> p) override {
    auto& m = table();
    const Packet& ref = *p;
    auto it = m.find(std::type_index(typeid(ref)));
    if (it != m.end()) {
      it->second(static_cast<Derived*>(this), p);
    }
  }

 private:
  using HandlerFn = void(*)(Derived*, std::shared_ptr<Packet>);

  static const std::unordered_map<std::type_index, HandlerFn>& table() {
    static const auto tbl = [] {
      std::unordered_map<std::type_index, HandlerFn> m;
      using expander = int[];
      (void)expander{0, (m.emplace(std::type_index(typeid(PacketTypes)), &call<PacketTypes>), 0)...};
      return m;
    }();
    return tbl;
  }

  // tag dispatch rather than `if constexpr`, which is C++17. both compile to
  // the same thing: the false_type overloads have empty bodies and inline away.
  template<typename P>
  static void CallConst(Derived* self, const std::shared_ptr<P>& p,
                        std::true_type) {
    self->OnPacket(static_cast<const P&>(*p));
  }
  template<typename P>
  static void CallConst(Derived*, const std::shared_ptr<P>&, std::false_type) {}

  template<typename P>
  static void CallShared(Derived* self, const std::shared_ptr<P>& p,
                         std::true_type) {
    self->OnPacket(p);
  }
  template<typename P>
  static void CallShared(Derived*, const std::shared_ptr<P>&, std::false_type) {}

  // main dispatcher
  template<typename P>
  static void call(Derived* self, std::shared_ptr<Packet> p_base) {
    auto p = std::static_pointer_cast<P>(p_base);

    CallConst<P>(self, p, detail::HasOnPacketConstT<Derived, P>{});
    CallShared<P>(self, p, detail::HasOnPacketSharedT<Derived, P>{});
  }

};

class CallbackPacketHandler : public PacketHandlerBase {
  using SharedHandlerFn = std::function<void(std::shared_ptr<Packet>)>;
  using RefHandlerFn = std::function<void(const Packet&)>;

  std::unordered_map<std::type_index, SharedHandlerFn> sharedHandlers;
  std::unordered_map<std::type_index, RefHandlerFn> refHandlers;

 public:
  ZNET_TPL_DERIVED_FROM_PACKET(T)
  void AddShared(std::function<void(std::shared_ptr<T>)> fn) {
    sharedHandlers[typeid(T)] = [fn](std::shared_ptr<Packet> p) {
      fn(std::static_pointer_cast<T>(p));
    };
  }

  ZNET_TPL_DERIVED_FROM_PACKET(T)
  void AddRef(std::function<void(const T&)> fn) {
    refHandlers[typeid(T)] = [fn](std::shared_ptr<Packet> p) {
      fn(*std::static_pointer_cast<T>(p));
    };
  }

  void Handle(std::shared_ptr<Packet> p) override {
    const Packet& ref = *p;
    auto type = std::type_index(typeid(ref));

    auto sharedIt = sharedHandlers.find(type);
    if (sharedIt != sharedHandlers.end()) {
      sharedIt->second(p);
      return;
    }

    auto refIt = refHandlers.find(type);
    if (refIt != refHandlers.end()) {
      refIt->second(*p);
      return;
    }

    // todo fallback or log unknown packet
  }
};


}  // namespace znet