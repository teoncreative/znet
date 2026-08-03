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
// Inside the authentication service. In a real deployment this is a machine you
// operate and neither the game client nor the game server can reach.
//

#include "service.h"

namespace {

// A fixed keypair so the two example programs agree without a key exchange of
// their own. A real service generates this once, keeps the private half in an
// HSM or a secrets manager, and publishes only the public half.
const unsigned char kPublicKey[kEd25519KeyLength] = {
    0x8c, 0x69, 0x83, 0xe1, 0x0a, 0xfc, 0x55, 0xac,
    0x6a, 0x19, 0x9b, 0x05, 0x6b, 0x4d, 0x74, 0xa2,
    0xd6, 0x35, 0x97, 0xdc, 0xd6, 0x56, 0x5a, 0x73,
    0x9e, 0xd2, 0xed, 0x35, 0x06, 0xdb, 0x8c, 0x72,
};

// Declared in no header on purpose. Anything holding this can mint any
// identity, so in the real system it exists on one machine and is never copied.
const unsigned char kPrivateKey[kEd25519KeyLength] = {
    0xb4, 0x89, 0xad, 0x5a, 0x35, 0xf5, 0x93, 0x35,
    0x98, 0xef, 0x8c, 0xa2, 0xcd, 0x7d, 0xcb, 0x94,
    0x7e, 0x2e, 0x7c, 0x61, 0x35, 0x07, 0x41, 0x83,
    0x55, 0xa2, 0x46, 0x51, 0xc6, 0x01, 0x6f, 0x95,
};

// Short, because a token is a standing claim about a user and the only bound on
// a leaked one is how soon it stops being accepted.
constexpr int64_t kTokenLifetimeSeconds = 300;

}  // namespace

const unsigned char* ServicePublicKey() {
  return kPublicKey;
}

bool RequestToken(const std::string& user_id,
                  const unsigned char* client_public_key, AuthToken* out) {
  if (!client_public_key || !out) {
    return false;
  }
  // A real service authenticates the user here, by password, OAuth, a console
  // account, whatever it owns. Everything downstream trusts this step, and the
  // example skips it entirely.
  out->user_id = user_id;
  memcpy(out->client_public_key, client_public_key, kEd25519KeyLength);
  out->expires_at = NowSeconds() + kTokenLifetimeSeconds;
  return Ed25519Sign(kPrivateKey, out->SignedPart(), out->service_signature);
}
