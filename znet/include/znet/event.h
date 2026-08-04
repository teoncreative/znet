//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_EVENT_H_
#define ZNET_EVENT_H_

#include "znet/precompiled.h"

namespace znet {

// bit flags, so one event can belong to several categories
enum EventCategory {
  EventCategoryServer = 1 << 0,
  EventCategoryClient = 1 << 1,
  EventCategoryP2P = 1 << 2,
};

class Event {
 public:
  virtual ~Event() = default;

  virtual const char* GetEventName() const = 0;
  virtual size_t GetEventType() const = 0;
  virtual int GetCategoryFlags() const = 0;

  ZNET_NODISCARD bool IsInCategory(EventCategory category) const {
    return (GetCategoryFlags() & category) != 0;
  }

  /** @brief Whether a dispatched handler returned true for this event. */
  ZNET_NODISCARD bool handled() const { return handled_; }

 private:
  friend class EventDispatcher;
  bool handled_ = false;
};

using EventCallbackFn = std::function<void(Event&)>;

class EventDispatcher {
 public:
  explicit EventDispatcher(Event& event) : event_(event) {}

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

#endif  // ZNET_EVENT_H_
