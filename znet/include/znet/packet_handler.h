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
    }
  }

 private:
  using HandlerFn = void(*)(Derived*, std::shared_ptr<Packet>);

  // traits for OnPacket(const P&)
  template<typename T, typename P, typename = void>
  struct has_pkt_const : std::false_type {};
  template<typename T, typename P>
  struct has_pkt_const<T,P,void_t<
                                 decltype(std::declval<T>().OnPacket(std::declval<const P&>()))
                                 >> : std::true_type {};

  // traits for OnPacket(shared_ptr<P>)
  template<typename T, typename P, typename = void>
  struct has_pkt_shared : std::false_type {};
  template<typename T, typename P>
  struct has_pkt_shared<T,P,void_t<
                                  decltype(std::declval<T>().OnPacket(std::declval<std::shared_ptr<P>>()))
                                  >> : std::true_type {};

  static const std::unordered_map<std::type_index, HandlerFn>& table() {
    static const auto tbl = [] {
      std::unordered_map<std::type_index, HandlerFn> m;
      using expander = int[];
      (void)expander{0, (m.emplace(std::type_index(typeid(PacketTypes)), &call<PacketTypes>), 0)...};
      return m;
    }();
    return tbl;
  }

  // main dispatcher
  template<typename P>
  static void call(Derived* self, std::shared_ptr<Packet> p_base) {
    auto p = std::static_pointer_cast<P>(p_base);
    call_pkt_const <Derived,P>(self, p, has_pkt_const <Derived,P>{});
    call_pkt_shared<Derived,P>(self, p, has_pkt_shared<Derived,P>{});
  }

  // tag-dispatchers for OnPacket
  template<typename D, typename P>
  static void call_pkt_const(D* self, std::shared_ptr<P> p, std::true_type) {
    self->OnPacket(static_cast<const P&>(*p));
  }

  template<typename D, typename P>
  static void call_pkt_const(D*, std::shared_ptr<P>, std::false_type) {}

  template<typename D, typename P>
  static void call_pkt_shared(D* self, std::shared_ptr<P> p, std::true_type) {
    self->OnPacket(p);
  }

  template<typename D, typename P>
  static void call_pkt_shared(D*, std::shared_ptr<P>, std::false_type) {}

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