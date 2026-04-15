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

// Packet handler type constraints
template<typename T, typename P>
concept HasOnPacketConst = requires(T handler, const P& pkt) {
  { handler.OnPacket(pkt) } -> std::same_as<void>;
};

template<typename T, typename P>
concept HasOnPacketShared = requires(T handler, std::shared_ptr<P> pkt) {
  { handler.OnPacket(pkt) } -> std::same_as<void>;
};

template<typename T>
concept DerivedFromPacket = std::is_base_of_v<Packet, T>;

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

    if constexpr (HasOnPacketConst<Derived, P>) {
      self->OnPacket(static_cast<const P&>(*p));
    }

    if constexpr (HasOnPacketShared<Derived, P>) {
      self->OnPacket(p);
    }
  }

};

class CallbackPacketHandler : public PacketHandlerBase {
  using SharedHandlerFn = std::function<void(std::shared_ptr<Packet>)>;
  using RefHandlerFn = std::function<void(const Packet&)>;

  std::unordered_map<std::type_index, SharedHandlerFn> sharedHandlers;
  std::unordered_map<std::type_index, RefHandlerFn> refHandlers;

 public:
  template <DerivedFromPacket T>
  void AddShared(std::function<void(std::shared_ptr<T>)> fn) {
    sharedHandlers[typeid(T)] = [fn](std::shared_ptr<Packet> p) {
      fn(std::static_pointer_cast<T>(p));
    };
  }

  template <DerivedFromPacket T>
  void AddRef(std::function<void(const T&)> fn) {
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