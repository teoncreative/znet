//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"

namespace znet {

#define BIT(x) (1 << x)

enum EventCategory {
  EventCategoryServer = BIT(0),
  EventCategoryClient = BIT(1),
  EventCategoryP2P = BIT(1),
  EventCategoryUser = BIT(2)
};

#undef BIT

class Event {
 public:
  virtual ~Event() = default;

  bool handled_ = false;

  virtual const char* GetEventName() const = 0;
  virtual size_t GetEventType() const = 0;
  virtual int GetCategoryFlags() const = 0;

  bool IsInCategory(EventCategory category) {
    return GetCategoryFlags() & category;
  }
};

using EventCallbackFn = std::function<void(Event&)>;

class EventDispatcher {
 public:
  EventDispatcher(Event& event) : event_(event) {}

  // F will be deduced by the compiler
  template <typename T, typename F>
  bool Dispatch(const F& func) {
    if (event_.GetEventType() == T::GetStaticType()) {
      event_.handled_ |= func(static_cast<T&>(event_));
      return true;
    }
    return false;
  }

 private:
  Event& event_;
};

#define ZNET_EVENT_CLASS_TYPE(type)                   \
  static size_t GetStaticType() {                     \
    return typeid(type).hash_code();                  \
  }                                                   \
  virtual size_t GetEventType() const override {      \
    return GetStaticType();                           \
  }                                                   \
  virtual const char* GetEventName() const override { \
    return #type;                                     \
  }

#define ZNET_EVENT_CLASS_CATEGORY(category)       \
  virtual int GetCategoryFlags() const override { \
    return category;                              \
  }

}  // namespace znet