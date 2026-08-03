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
// Ed25519 over OpenSSL. Nothing znet-specific, and nothing secret: this is the
// signing everyone in the example does, the service over tokens and the client
// over its session binding.
//

#pragma once

#include <openssl/evp.h>

#include <cstddef>
#include <vector>

static constexpr size_t kEd25519KeyLength = 32;
static constexpr size_t kEd25519SigLength = 64;

inline bool Ed25519Sign(const unsigned char* private_key,
                        const std::vector<unsigned char>& message,
                        unsigned char* signature_out) {
  EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               private_key, kEd25519KeyLength);
  if (!key) {
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  size_t len = kEd25519SigLength;
  // Ed25519 signs in one shot: no DigestUpdate, and the digest is chosen for us
  const bool ok = ctx &&
                  EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) > 0 &&
                  EVP_DigestSign(ctx, signature_out, &len, message.data(),
                                 message.size()) > 0 &&
                  len == kEd25519SigLength;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

inline bool Ed25519Verify(const unsigned char* public_key,
                          const std::vector<unsigned char>& message,
                          const unsigned char* signature) {
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              public_key, kEd25519KeyLength);
  if (!key) {
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const bool ok =
      ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) > 0 &&
      EVP_DigestVerify(ctx, signature, kEd25519SigLength, message.data(),
                       message.size()) > 0;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

inline bool GenerateEd25519(unsigned char* public_out,
                            unsigned char* private_out) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  EVP_PKEY* key = nullptr;
  if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &key) <= 0) {
    EVP_PKEY_CTX_free(ctx);
    return false;
  }
  size_t public_len = kEd25519KeyLength;
  size_t private_len = kEd25519KeyLength;
  const bool ok =
      EVP_PKEY_get_raw_public_key(key, public_out, &public_len) > 0 &&
      EVP_PKEY_get_raw_private_key(key, private_out, &private_len) > 0;
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(ctx);
  return ok;
}
