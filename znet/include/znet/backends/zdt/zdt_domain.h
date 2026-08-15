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
// A debug-only check that a group of methods is never entered by two threads at
// once. ZDT's threading rules were written as comments, which is how a race in
// the session's encode path survived long enough to show up as a bimodal
// benchmark rather than as a failure. These turn the rules into something that
// aborts instead.
//

#ifndef ZNET_BACKENDS_ZDT_ZDT_DOMAIN_H_
#define ZNET_BACKENDS_ZDT_ZDT_DOMAIN_H_

#include "znet/compat.h"

#include <cassert>

#ifndef NDEBUG
#include <atomic>
#include <thread>
#endif

namespace znet {
namespace backends {

#ifndef NDEBUG

/**
 * @brief The owner slot a ThreadDomainGuard claims. One per domain.
 *
 * Declare it inside `#ifndef NDEBUG` in the owning class, so a release build
 * has no member rather than an unused one and the object layout is unchanged.
 */
struct ThreadDomain {
  std::atomic<std::thread::id> owner{std::thread::id{}};
};

/**
 * @brief Asserts mutual exclusion over a domain for as long as it is in scope.
 *
 * Deliberately *not* a thread-affinity check. A session legitimately changes
 * threads: the server ticks it on the acceptor thread while it is still
 * pending, then on a pool worker once promoted, so latching the first thread id
 * would fire on every accepted connection. What the code actually requires is
 * that no two threads are inside at the same time, which is what this checks.
 *
 * Re-entrant, because Update() calls Flush(): an inner scope recognizes itself
 * as the owner and leaves the slot claimed on the way out, so only the
 * outermost guard releases it.
 */
class ThreadDomainGuard {
 public:
  explicit ThreadDomainGuard(ThreadDomain& domain) : domain_(domain) {
    const std::thread::id self = std::this_thread::get_id();
    const std::thread::id previous =
        domain_.owner.exchange(self, std::memory_order_acq_rel);
    reentrant_ = previous == self;
    assert((previous == std::thread::id{} || reentrant_) &&
           "two threads entered one ZDT thread domain at once");
  }

  ~ThreadDomainGuard() {
    if (!reentrant_) {
      domain_.owner.store(std::thread::id{}, std::memory_order_release);
    }
  }

  ThreadDomainGuard(const ThreadDomainGuard&) = delete;
  ThreadDomainGuard& operator=(const ThreadDomainGuard&) = delete;

 private:
  ThreadDomain& domain_;
  bool reentrant_ = false;
};

// One guard per function scope, so a fixed name is enough and avoids the
// two-level macro dance __LINE__ pasting would otherwise need.
#define ZNET_ZDT_ENTER_DOMAIN(member) \
  ::znet::backends::ThreadDomainGuard znet_zdt_domain_guard_(member)

#else  // NDEBUG

#define ZNET_ZDT_ENTER_DOMAIN(member) ((void)0)

#endif  // NDEBUG

}  // namespace backends
}  // namespace znet


#endif  // ZNET_BACKENDS_ZDT_ZDT_DOMAIN_H_
