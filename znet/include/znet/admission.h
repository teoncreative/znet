//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_ADMISSION_H_
#define ZNET_ADMISSION_H_

#include "znet/compat.h"
#include "znet/inet_addr.h"
#include "znet/options.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace znet {
namespace backends {

/**
 * @brief Listener-side admission rules from ServerOptions: the allow and deny
 *        lists, and the per-source connection-attempt throttle.
 *
 * Owned by one server backend and driven from its accept path, so nothing
 * here is synchronized. The rules are fixed at construction and apply to new
 * connections only; sessions already accepted are never re-checked.
 */
class AdmissionControl {
 public:
  enum class Verdict { Allow, Denylisted, NotAllowlisted, Throttled };

  explicit AdmissionControl(const ServerOptions& options);

  /** @brief The lists alone; records nothing. */
  Verdict Screen(const InetAddress& source);

  /**
   * @brief The lists plus the attempt throttle; counts one attempt.
   *
   * Call once per connection attempt the backend sees: a TCP accept, or a
   * datagram that opens a ZDT handshake.
   */
  Verdict Admit(const InetAddress& source);

 private:
  struct SourceWindow {
    std::chrono::steady_clock::time_point window_start;
    uint32_t count = 0;
  };

  std::vector<CIDRBlock> allowlist_;
  std::vector<CIDRBlock> denylist_;
  uint32_t max_attempts_;
  std::chrono::milliseconds window_;
  std::unordered_map<std::string, SourceWindow> sources_;
};

inline std::string GetVerdictString(AdmissionControl::Verdict verdict) {
  switch (verdict) {
    case AdmissionControl::Verdict::Allow:
      return "Allow";
    case AdmissionControl::Verdict::Denylisted:
      return "Denylisted";
    case AdmissionControl::Verdict::NotAllowlisted:
      return "NotAllowlisted";
    case AdmissionControl::Verdict::Throttled:
      return "Throttled";
    default:
      return "Unknown";
  }
}

}  // namespace backends
}  // namespace znet


#endif  // ZNET_ADMISSION_H_
