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

#include "znet/precompiled.h"
#include "znet/base/packet.h"
#include "znet/packet_serializer.h"
#include "znet/logger.h"

namespace znet {

// I hate this but since we dont have constexpr and other good features, in C++14
// We cannot have something better for once...
// I'll update this to work
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
    auto it = m.find(std::type_index(typeid(*p)));
    if (it != m.end()) {
      it->second(static_cast<Derived*>(this), p);
    } else {
      call_unknown(static_cast<Derived*>(this), p);
    }
  }

 private:
  using HandlerFn = void(*)(Derived*, std::shared_ptr<Packet>);

  template <typename T, typename Arg>
  struct has_const_ref {
   private:
    template <typename U>
    static auto test(int) -> decltype(std::declval<U>().OnPacket(std::declval<const Arg&>()), std::true_type{});
    template <typename>
    static std::false_type test(...);
   public:
    static constexpr bool value = decltype(test<T>(0))::value;
  };

  template <typename T, typename Arg>
  struct has_shared_ptr {
   private:
    template <typename U>
    static auto test(int) -> decltype(std::declval<U>().OnPacket(std::declval<std::shared_ptr<Arg>>()), std::true_type{});
    template <typename>
    static std::false_type test(...);
   public:
    static constexpr bool value = decltype(test<T>(0))::value;
  };

  static const std::unordered_map<std::type_index, HandlerFn>& table() {
    static const auto tbl = [] {
      std::unordered_map<std::type_index, HandlerFn> m;
      using expander = int[];
      (void)expander{0, (m.emplace(std::type_index(typeid(PacketTypes)), &call<PacketTypes>), 0)...};
      return m;
    }();
    return tbl;
  }

  template<typename P>
  static void call(Derived* self, std::shared_ptr<Packet> p) {
    call_impl<P>(
        self, p,
        std::integral_constant<bool, has_const_ref<Derived, P>::value>{},
        std::integral_constant<bool, has_shared_ptr<Derived, P>::value>{});
  }

  template<typename P>
  static void call_impl(Derived* self, std::shared_ptr<Packet> p, std::true_type, std::false_type) {
    self->OnPacket((const P&) *std::static_pointer_cast<P>(p));
  }

  template<typename P>
  static void call_impl(Derived* self, std::shared_ptr<Packet> p, std::false_type, std::true_type) {
    self->OnPacket(std::static_pointer_cast<P>(p));
  }

  template<typename P>
  static void call_impl(Derived* self, std::shared_ptr<Packet> p, std::true_type, std::true_type) {
    self->OnPacket((const P&) *std::static_pointer_cast<P>(p));
    self->OnPacket(std::static_pointer_cast<P>(p));
  }

  template<typename P>
  static void call_impl(Derived* self, std::shared_ptr<Packet> p, std::false_type, std::false_type) {
  }

  static void call_unknown(Derived* self, std::shared_ptr<Packet> p) {
    constexpr bool has_const_ref = is_invocable<decltype(&Derived::OnUnknown), Derived*, const Packet&>::value;
    constexpr bool has_shared = is_invocable<decltype(&Derived::OnUnknown), Derived*, std::shared_ptr<Packet>>::value;

    call_unknown_impl(self, p, std::integral_constant<bool, has_const_ref>{}, std::integral_constant<bool, has_shared>{});
  }

  static void call_unknown_impl(Derived* self, std::shared_ptr<Packet> p, std::true_type, std::false_type) {
    self->OnUnknown(*p);
  }

  static void call_unknown_impl(Derived* self, std::shared_ptr<Packet> p, std::false_type, std::true_type) {
    self->OnUnknown(p);
  }

  static void call_unknown_impl(Derived* self, std::shared_ptr<Packet> p, std::true_type, std::true_type) {
    self->OnUnknown(*p);
    self->OnUnknown(p);
  }

  static void call_unknown_impl(Derived* self, std::shared_ptr<Packet> p, std::false_type, std::false_type) {

  }
};

class CallbackPacketHandler : public PacketHandlerBase {
  using SharedHandlerFn = std::function<void(std::shared_ptr<Packet>)>;
  using RefHandlerFn = std::function<void(const Packet&)>;

  std::unordered_map<std::type_index, SharedHandlerFn> sharedHandlers;
  std::unordered_map<std::type_index, RefHandlerFn> refHandlers;

 public:
  template <typename T>
  void AddShared(std::function<void(std::shared_ptr<T>)> fn) {
    static_assert(std::is_base_of<Packet, T>::value, "T must derive from Packet");
    sharedHandlers[typeid(T)] = [fn](std::shared_ptr<Packet> p) {
      fn(std::static_pointer_cast<T>(p));
    };
  }

  template <typename T>
  void AddRef(std::function<void(const T&)> fn) {
    static_assert(std::is_base_of<Packet, T>::value, "T must derive from Packet");
    refHandlers[typeid(T)] = [fn](std::shared_ptr<Packet> p) {
      fn(*std::static_pointer_cast<T>(p));
    };
  }

  void Handle(std::shared_ptr<Packet> p) override {
    auto type = std::type_index(typeid(*p));

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