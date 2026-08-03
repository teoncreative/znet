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
// What the service says about a user. Shared by all three parties, since the
// service writes one, the client carries it and the server checks it, but note
// that checking needs only a public key. Nothing here can mint a token.
//

#pragma once

#include "crypto.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

inline int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// "The service says this user holds this key, until this time."
//
// Note what is *not* here: anything about a session. The token is reusable by
// design, which is exactly why it cannot be the whole proof. Binding it to one
// session is the client's job, and the export is what it binds to.
struct AuthToken {
  std::string user_id;
  unsigned char client_public_key[kEd25519KeyLength] = {};
  int64_t expires_at = 0;
  unsigned char service_signature[kEd25519SigLength] = {};

  // What the service signs. Length-prefixed, so a user_id cannot be shifted
  // into the key and produce the same bytes.
  std::vector<unsigned char> SignedPart() const {
    std::vector<unsigned char> out;
    const uint32_t id_len = static_cast<uint32_t>(user_id.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<unsigned char>((id_len >> shift) & 0xFFu));
    }
    out.insert(out.end(), user_id.begin(), user_id.end());
    out.insert(out.end(), client_public_key,
               client_public_key + kEd25519KeyLength);
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<unsigned char>((expires_at >> shift) & 0xFF));
    }
    return out;
  }

  bool VerifiedBy(const unsigned char* service_public_key) const {
    if (expires_at <= NowSeconds()) {
      return false;
    }
    return Ed25519Verify(service_public_key, SignedPart(), service_signature);
  }
};
