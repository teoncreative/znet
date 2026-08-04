//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/admission.h"

#include "znet/logger.h"

namespace znet {
namespace backends {

namespace {

// bounds the throttle table. When it is full of live windows, unseen sources
// pass uncounted: failing open is deliberate, since an attacker rotating
// addresses defeats a per-source counter anyway, and failing closed would let
// that same rotation lock genuine sources out.
constexpr size_t kMaxTrackedSources = 4096;

bool AnyMatch(const std::vector<CIDRBlock>& blocks, const InetAddress& source) {
  for (const auto& block : blocks) {
    if (block.Matches(source)) {
      return true;
    }
  }
  return false;
}

std::vector<CIDRBlock> ValidOnly(const std::vector<CIDRBlock>& blocks,
                                 const char* which) {
  std::vector<CIDRBlock> out;
  out.reserve(blocks.size());
  for (const auto& block : blocks) {
    if (block.is_valid()) {
      out.push_back(block);
    } else {
      // dropped loudly: a rule kept silently would admit or refuse traffic
      // the config never meant to
      ZNET_LOG_ERROR("Ignoring invalid CIDR block \"{}\" in the {}.",
                     block.text(), which);
    }
  }
  return out;
}

}  // namespace

AdmissionControl::AdmissionControl(const ServerOptions& options)
    : allowlist_(ValidOnly(options.allowlist, "allowlist")),
      denylist_(ValidOnly(options.denylist, "denylist")),
      max_attempts_(options.max_attempts_per_source),
      window_(options.attempt_window) {}

AdmissionControl::Verdict AdmissionControl::Screen(const InetAddress& source) {
  if (source.ipv() == InetProtocolVersion::Unix) {
    // no address to filter; the socket file's permissions are the gate
    return Verdict::Allow;
  }
  if (AnyMatch(denylist_, source)) {
    return Verdict::Denylisted;
  }
  if (!allowlist_.empty() && !AnyMatch(allowlist_, source)) {
    return Verdict::NotAllowlisted;
  }
  return Verdict::Allow;
}

AdmissionControl::Verdict AdmissionControl::Admit(const InetAddress& source) {
  const Verdict verdict = Screen(source);
  if (verdict != Verdict::Allow) {
    return verdict;
  }
  if (max_attempts_ == 0 || window_.count() <= 0) {
    return Verdict::Allow;
  }
  // host_key() drops the port, so every connection from one host lands on one
  // entry; empty means there is no address to key on (a unix path)
  const std::string key = source.host_key();
  if (key.empty()) {
    return Verdict::Allow;
  }
  const auto now = std::chrono::steady_clock::now();
  if (sources_.size() > kMaxTrackedSources) {
    for (auto it = sources_.begin(); it != sources_.end();) {
      if (now - it->second.window_start > window_) {
        it = sources_.erase(it);
      } else {
        ++it;
      }
    }
    if (sources_.size() > kMaxTrackedSources &&
        sources_.find(key) == sources_.end()) {
      return Verdict::Allow;  // full of live windows; see kMaxTrackedSources
    }
  }
  SourceWindow& entry = sources_[key];
  if (entry.count == 0 || now - entry.window_start > window_) {
    entry.window_start = now;
    entry.count = 0;
  }
  entry.count++;
  return entry.count <= max_attempts_ ? Verdict::Allow : Verdict::Throttled;
}


}  // namespace backends
}  // namespace znet
