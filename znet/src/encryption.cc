//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/encryption.h"
#include "znet/peer_session.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <vector>

namespace znet {

/*void print_hex(const unsigned char* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    if (i < length - 1) std::cout << ":";
    if ((i + 1) % 16 == 0 && i != 0 && i < length - 1) std::cout << "\n";
  }
  std::cout << std::dec << std::endl; // Switch back to decimal for any further numbers
}

void print_key(EVP_PKEY* key) {
  BIO* fp = BIO_new_fp(stdout, BIO_NOCLOSE);
  EVP_PKEY_print_public(fp, key, 0, NULL);
}*/

unsigned char* SerializePublicKey(EVP_PKEY* pkey, uint32_t* len) {
  if (!pkey || !len) {
    return nullptr;
  }

  unsigned char* der = nullptr;
  int der_len = i2d_PUBKEY(pkey, &der);  // Serialize the public key to DER format
  if (der_len <= 0) {
    ZNET_LOG_ERROR("Failed to serialize public key.");
    if (der) {
      OPENSSL_free(der);
    }
    return nullptr;
  }
  *len = static_cast<uint32_t>(der_len);

  return der;  // The caller must free this memory using OPENSSL_free
}

UniquePKey DeserializePublicKey(const unsigned char* der, uint32_t len) {
  if (!der || len <= 0) {
    return nullptr;
  }

  const unsigned char* p = der;
  EVP_PKEY* raw = d2i_PUBKEY(nullptr, &p, static_cast<long>(len));
  if (!raw) {
    ZNET_LOG_ERROR("Failed to deserialize public key.");
    return nullptr;
  }

  return UniquePKey(raw);
}

UniquePKey CloneKey(const UniquePKey& k) {
  if (!k) {
    return {};
  }
  // up-ref so the packet owns its own reference
  if (EVP_PKEY_up_ref(k.get()) <= 0) {
    ZNET_LOG_ERROR("Failed to up_ref pub_key");
    return {};
  }
  return UniquePKey(k.get());
}

UniquePKey GenerateKey() {
  /* Create the context for generating the parameters */
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_DH, nullptr);
  if (!pctx) {
    return nullptr;
  }

  if (!EVP_PKEY_paramgen_init(pctx)) {
    EVP_PKEY_CTX_free(pctx);
    return nullptr;
  }

  /* Set a prime length of 2048 */
  if (!EVP_PKEY_CTX_set_dh_paramgen_prime_len(pctx, 2048)) {
    EVP_PKEY_CTX_free(pctx);
    return nullptr;
  }

  /* Use built-in parameters */
  EVP_PKEY* params = EVP_PKEY_new();
  if (params == nullptr) {
    EVP_PKEY_CTX_free(pctx);
    return nullptr;
  }

  if (EVP_PKEY_assign(params, EVP_PKEY_DHX, DH_get_2048_256()) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return nullptr;
  }

  EVP_PKEY_CTX_free(pctx);

  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new(params, nullptr);
  /* Create context for the key generation */
  if (!kctx) {
    EVP_PKEY_CTX_free(kctx);
    EVP_PKEY_free(params);
    return nullptr;
  }

  /* Generate a new key */
  if (EVP_PKEY_keygen_init(kctx) <= 0) {
    EVP_PKEY_CTX_free(kctx);
    EVP_PKEY_free(params);
    return nullptr;
  }

  EVP_PKEY* dhkey = nullptr;
  if (EVP_PKEY_keygen(kctx, &dhkey) <= 0) {
    EVP_PKEY_CTX_free(kctx);
    EVP_PKEY_free(params);
    return nullptr;
  }
  EVP_PKEY_CTX_free(kctx);
  EVP_PKEY_free(params);

  //print_key(dhkey);
  return UniquePKey(dhkey);
}

namespace {

// Wire layout of an encrypted message, after the mode byte:
//   [stream:1][counter:7][ciphertext:n][tag:16]
// `stream` is the ordering domain the transport put this message in (see
// TransportLayer::OrderingDomain); the cipher never interprets it beyond
// scoping a sequence to it.
// The nonce is salt || stream || counter; the salt never goes on the wire,
// both sides derive it from the shared secret. Counters run per stream, so
// the stream has to be in the nonce for (key, nonce) pairs to stay unique.
// It needs no separate authentication: altering it derives a different nonce,
// and the tag then fails. GCM is a stream cipher, so the ciphertext is exactly
// as long as the plaintext.
constexpr size_t kNonceLen = 12;  // 4 salt + 1 stream + 7 counter
constexpr size_t kNonceSaltLen = 4;
constexpr size_t kCounterLen = 7;
constexpr size_t kHeaderLen = 1 + kCounterLen;  // stream + counter, on the wire
constexpr size_t kTagLen = 16;
// 7 bytes of counter: 2^56 messages on one stream before it would repeat.
constexpr uint64_t kMaxCounter = (uint64_t{1} << (8 * kCounterLen)) - 1;

constexpr uint8_t kModePlaintext = 0;
constexpr uint8_t kModeAesCbc = 1;  // retired: unauthenticated, see HandleDecrypt
constexpr uint8_t kModeAesGcm = 2;

// Big-endian, so a packet capture reads in order.
void WriteCounter(unsigned char* out, uint64_t counter) {
  for (size_t i = 0; i < kCounterLen; i++) {
    out[i] = static_cast<unsigned char>(
        (counter >> (8 * (kCounterLen - 1 - i))) & 0xFFu);
  }
}

uint64_t ReadCounter(const unsigned char* in) {
  uint64_t counter = 0;
  for (size_t i = 0; i < kCounterLen; i++) {
    counter = (counter << 8) | in[i];
  }
  return counter;
}

void BuildNonce(const unsigned char* salt, uint8_t stream, uint64_t counter,
                unsigned char* out) {
  memcpy(out, salt, kNonceSaltLen);
  out[kNonceSaltLen] = stream;
  WriteCounter(out + kNonceSaltLen + 1, counter);
}

}  // namespace

unsigned char* ComputeSharedSecret(EVP_PKEY* pkey, EVP_PKEY* peer_pkey,
                                   size_t* secret_len) {
  if (!pkey || !peer_pkey || !secret_len) {
    ZNET_LOG_ERROR("Invalid input to ComputeSharedSecret.");
    return nullptr;
  }

  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
  if (!ctx) {
    ZNET_LOG_ERROR("Failed to create EVP_PKEY_CTX.");
    return nullptr;
  }

  if (EVP_PKEY_derive_init(ctx) <= 0) {
    ZNET_LOG_ERROR("Failed to initialize derive context.");
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  if (EVP_PKEY_derive_set_peer(ctx, peer_pkey) <= 0) {
    ZNET_LOG_ERROR("Failed to set peer key.");
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  if (EVP_PKEY_derive(ctx, nullptr, secret_len) <= 0) {
    ZNET_LOG_ERROR("Failed to determine shared secret length.");
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  unsigned char* secret = new unsigned char[*secret_len];
  if (!secret) {
    ZNET_LOG_ERROR("Failed to allocate memory for shared secret.");
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  if (EVP_PKEY_derive(ctx, secret, secret_len) <= 0) {
    ZNET_LOG_ERROR("Failed to derive shared secret.");
    delete[] secret;
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  EVP_PKEY_CTX_free(ctx);
  return secret;
}

// One HKDF-SHA256 expansion. `info` is the domain separator: two expansions of
// one secret under different info are independent, which is what lets the
// directional keys and the exporter share a root without sharing anything else.
bool Hkdf(const unsigned char* secret, size_t secret_len,
          const unsigned char* info, size_t info_len, unsigned char* out,
          size_t out_len) {
  if (secret_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      info_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      out_len > static_cast<size_t>(std::numeric_limits<int>::max())) {
    ZNET_LOG_ERROR("Secret, info or key length too large");
    return false;
  }

  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!pctx) {
    ZNET_LOG_ERROR("Failed to create EVP_PKEY_CTX for HKDF.");
    return false;
  }

  static const unsigned char kSalt[] = "znet-session-v1";
  size_t derived_len = out_len;  // EVP_PKEY_derive needs size_t*
  if (EVP_PKEY_derive_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(pctx, kSalt,
                                  static_cast<int>(sizeof(kSalt) - 1)) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(pctx, secret,
                                 static_cast<int>(secret_len)) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(pctx, info, static_cast<int>(info_len)) <= 0 ||
      EVP_PKEY_derive(pctx, out, &derived_len) <= 0) {
    ZNET_LOG_ERROR("Failed to derive key using HKDF.");
    EVP_PKEY_CTX_free(pctx);
    return false;
  }

  EVP_PKEY_CTX_free(pctx);
  return true;
}

// `label` separates the two directions, so the client-to-server and
// server-to-client streams never share a key: with a counter nonce, one key used
// both ways would repeat a (key, nonce) pair, which breaks GCM outright rather
// than merely weakening it.
bool DeriveKeyFromSharedSecret(const unsigned char* shared_secret,
                               size_t secret_len, const char* label,
                               unsigned char* key, size_t key_len) {
  return Hkdf(shared_secret, secret_len,
              reinterpret_cast<const unsigned char*>(label), strlen(label), key,
              key_len);
}

// `ctx` is owned by the caller and reused across messages. `set_key` is true
// only the first time, so the AES key schedule is derived once per session
// instead of once per message; later calls reset the nonce and nothing else.
//
// `aad` is authenticated but not encrypted: the mode byte goes through it, so a
// flipped mode fails the tag instead of steering the receiver somewhere else.
//
// Returns the ciphertext length, or -1 on failure. Zero is a valid length (an
// empty plaintext), which is why failure is not folded into it.
int EncryptData(EVP_CIPHER_CTX* ctx, bool set_key, const unsigned char* key,
                const unsigned char* nonce, const unsigned char* aad,
                int aad_len, const unsigned char* plaintext, int plaintext_len,
                unsigned char* ciphertext, unsigned char* tag) {
  if (!ctx) {
    ZNET_LOG_ERROR("Failed to create EVP_CIPHER_CTX.");
    return -1;
  }

  if (set_key) {
    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                                nullptr) ||
        1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                 static_cast<int>(kNonceLen), nullptr) ||
        1 != EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce)) {
      ZNET_LOG_ERROR("Failed to initialize encryption.");
      return -1;
    }
  } else if (1 != EVP_EncryptInit_ex(ctx, nullptr, nullptr, nullptr, nonce)) {
    ZNET_LOG_ERROR("Failed to set the encryption nonce.");
    return -1;
  }

  int scratch = 0;
  if (aad_len > 0 &&
      1 != EVP_EncryptUpdate(ctx, nullptr, &scratch, aad, aad_len)) {
    ZNET_LOG_ERROR("Failed to authenticate associated data.");
    return -1;
  }

  int ciphertext_len = 0;
  if (1 != EVP_EncryptUpdate(ctx, ciphertext, &ciphertext_len, plaintext,
                             plaintext_len)) {
    ZNET_LOG_ERROR("Failed to encrypt data.");
    return -1;
  }

  int len = 0;
  if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len)) {
    ZNET_LOG_ERROR("Failed to finalize encryption.");
    return -1;
  }
  ciphertext_len += len;

  if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                               static_cast<int>(kTagLen), tag)) {
    ZNET_LOG_ERROR("Failed to read the authentication tag.");
    return -1;
  }
  return ciphertext_len;
}

// reuses `ctx` the same way EncryptData does; see the note there.
//
// Returns the plaintext length, or -1 when the tag does not verify. A failure
// here means the message was forged, tampered with, or replayed under a
// different nonce, and the caller must drop it: nothing decrypted is
// trustworthy until EVP_DecryptFinal_ex has passed.
int DecryptData(EVP_CIPHER_CTX* ctx, bool set_key, const unsigned char* key,
                const unsigned char* nonce, const unsigned char* aad,
                int aad_len, const unsigned char* ciphertext,
                int ciphertext_len, const unsigned char* tag,
                unsigned char* plaintext) {
  if (!ctx) {
    ZNET_LOG_ERROR("Failed to create EVP_CIPHER_CTX");
    return -1;
  }

  if (set_key) {
    if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                                nullptr) ||
        1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                 static_cast<int>(kNonceLen), nullptr) ||
        1 != EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce)) {
      ZNET_LOG_ERROR("Failed to initialize decryption");
      return -1;
    }
  } else if (1 != EVP_DecryptInit_ex(ctx, nullptr, nullptr, nullptr, nonce)) {
    ZNET_LOG_ERROR("Failed to set the decryption nonce");
    return -1;
  }

  int scratch = 0;
  if (aad_len > 0 &&
      1 != EVP_DecryptUpdate(ctx, nullptr, &scratch, aad, aad_len)) {
    ZNET_LOG_ERROR("Failed to authenticate associated data");
    return -1;
  }

  int plaintext_len = 0;
  if (1 != EVP_DecryptUpdate(ctx, plaintext, &plaintext_len, ciphertext,
                             ciphertext_len)) {
    ZNET_LOG_ERROR("Failed to decrypt data");
    return -1;
  }

  // const_cast: OpenSSL's ctrl takes void*, but SET_TAG only reads from it
  if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                               static_cast<int>(kTagLen),
                               const_cast<unsigned char*>(tag))) {
    ZNET_LOG_ERROR("Failed to set the authentication tag");
    return -1;
  }

  int len = 0;
  if (1 != EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len)) {
    return -1;  // tag mismatch; the caller logs, this is attacker-triggerable
  }
  plaintext_len += len;
  return plaintext_len;
}

EncryptionLayer::EncryptionLayer(PeerSession& session) : session_(session) {
  pub_key_ = GenerateKey();
  if (!pub_key_) {
    ZNET_LOG_ERROR(
        "Failed to generate key for encryption, closing the connection!");
    session_.Close();
    return;
  }

  auto handler = std::make_shared<CallbackPacketHandler>();
  handler->AddShared<HandshakePacket>(ZNET_BIND_FN(OnHandshakePacket));
  handler->AddShared<ConnectionReadyPacket>(ZNET_BIND_FN(OnAcknowledgePacket));
  session_.SetHandler(std::move(handler));

  auto codec = std::make_shared<Codec>();
  codec->Add(HandshakePacket::GetPacketId(),
             std::make_unique<HandshakePacketSerializerV1>());
  codec->Add(ConnectionReadyPacket::GetPacketId(),
             std::make_unique<ConnectionReadyPacketSerializerV1>());
  session_.SetCodec(std::move(codec));
}

void EncryptionLayer::Initialize(bool send, bool want_encryption) {
  want_encryption_ = want_encryption;
  if (send) {
    SendHandshake();
  }
}

EncryptionLayer::~EncryptionLayer() {
  if (enc_ctx_) {
    EVP_CIPHER_CTX_free(enc_ctx_);
    enc_ctx_ = nullptr;
  }
  if (dec_ctx_) {
    EVP_CIPHER_CTX_free(dec_ctx_);
    dec_ctx_ = nullptr;
  }
  // key material, so it is wiped rather than just released: leaving it in
  // freed heap hands it to whatever allocates that block next.
  OPENSSL_cleanse(tx_key_, sizeof(tx_key_));
  OPENSSL_cleanse(rx_key_, sizeof(rx_key_));
  OPENSSL_cleanse(tx_salt_, sizeof(tx_salt_));
  OPENSSL_cleanse(rx_salt_, sizeof(rx_salt_));
  OPENSSL_cleanse(exporter_secret_, sizeof(exporter_secret_));
  if (shared_secret_) {
    OPENSSL_cleanse(shared_secret_, shared_secret_len_);
    delete[] shared_secret_;
    shared_secret_ = nullptr;
  }
}

// Expands the DH secret into one key+salt per direction. The initiator writes
// the client-to-server stream and reads the other; the acceptor is the mirror,
// so both sides agree on which label belongs to which end of the wire.
bool EncryptionLayer::DeriveDirectionalKeys() {
  static const char kClientToServer[] = "znet c2s v1";
  static const char kServerToClient[] = "znet s2c v1";
  const char* tx_label =
      session_.is_initiator() ? kClientToServer : kServerToClient;
  const char* rx_label =
      session_.is_initiator() ? kServerToClient : kClientToServer;

  // key and nonce salt come from one expansion per direction, so the salt is
  // as unpredictable as the key and never travels on the wire
  unsigned char tx_material[sizeof(tx_key_) + sizeof(tx_salt_)];
  unsigned char rx_material[sizeof(rx_key_) + sizeof(rx_salt_)];
  if (!DeriveKeyFromSharedSecret(shared_secret_, shared_secret_len_, tx_label,
                                 tx_material, sizeof(tx_material)) ||
      !DeriveKeyFromSharedSecret(shared_secret_, shared_secret_len_, rx_label,
                                 rx_material, sizeof(rx_material))) {
    OPENSSL_cleanse(tx_material, sizeof(tx_material));
    OPENSSL_cleanse(rx_material, sizeof(rx_material));
    return false;
  }
  memcpy(tx_key_, tx_material, sizeof(tx_key_));
  memcpy(tx_salt_, tx_material + sizeof(tx_key_), sizeof(tx_salt_));
  memcpy(rx_key_, rx_material, sizeof(rx_key_));
  memcpy(rx_salt_, rx_material + sizeof(rx_key_), sizeof(rx_salt_));
  OPENSSL_cleanse(tx_material, sizeof(tx_material));
  OPENSSL_cleanse(rx_material, sizeof(rx_material));
  return DeriveExporterSecret();
}

// The root the exporter expands from, bound to a transcript of both public keys
// as well as the secret they produced.
//
// The binding is the point. An interceptor runs two separate exchanges, one with
// each end, so it holds two different secrets over two different transcripts and
// cannot make the two sides agree on an exported value. That is what lets an
// application prove a credential belongs to *this* session rather than to any
// session, which an anonymous exchange cannot say on its own.
//
// Derived once here rather than per export, so the DH secret is not needed again
// and the transcript is hashed one time.
bool EncryptionLayer::DeriveExporterSecret() {
  // initiator first, so both ends hash the same bytes in the same order
  EVP_PKEY* initiator =
      session_.is_initiator() ? pub_key_.get() : peer_pkey_.get();
  EVP_PKEY* acceptor =
      session_.is_initiator() ? peer_pkey_.get() : pub_key_.get();

  uint32_t initiator_len = 0;
  uint32_t acceptor_len = 0;
  unsigned char* initiator_der = SerializePublicKey(initiator, &initiator_len);
  unsigned char* acceptor_der = SerializePublicKey(acceptor, &acceptor_len);
  if (!initiator_der || !acceptor_der) {
    OPENSSL_free(initiator_der);
    OPENSSL_free(acceptor_der);
    ZNET_LOG_ERROR("Failed to serialize a public key for the exporter.");
    return false;
  }

  // length-prefixed: plain concatenation would let a different pair of keys
  // hash to the same transcript
  std::vector<unsigned char> transcript;
  transcript.reserve(8 + initiator_len + acceptor_len);
  auto append = [&transcript](const unsigned char* der, uint32_t len) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      transcript.push_back(static_cast<unsigned char>((len >> shift) & 0xFFu));
    }
    transcript.insert(transcript.end(), der, der + len);
  };
  append(initiator_der, initiator_len);
  append(acceptor_der, acceptor_len);
  OPENSSL_free(initiator_der);
  OPENSSL_free(acceptor_der);

  // hashed rather than fed to HKDF whole: OpenSSL bounds the info it accepts,
  // and two DER-encoded DH keys can carry their group parameters past it
  unsigned char digest[32];
  unsigned int digest_len = 0;
  if (EVP_Digest(transcript.data(), transcript.size(), digest, &digest_len,
                 EVP_sha256(), nullptr) <= 0 ||
      digest_len != sizeof(digest)) {
    ZNET_LOG_ERROR("Failed to hash the exporter transcript.");
    return false;
  }

  static const char kExporterRoot[] = "znet exporter v1";
  std::vector<unsigned char> info(kExporterRoot,
                                  kExporterRoot + sizeof(kExporterRoot) - 1);
  info.insert(info.end(), digest, digest + sizeof(digest));
  return Hkdf(shared_secret_, shared_secret_len_, info.data(), info.size(),
              exporter_secret_, sizeof(exporter_secret_));
}

Result EncryptionLayer::ExportKeyingMaterial(const std::string& label,
                                             unsigned char* out,
                                             size_t out_len) const {
  // one answer for "unencrypted" and "handshake unfinished": the flags cannot
  // tell them apart before the exchange settles, so the split would lie
  if (!enable_encryption_ || !key_filled_) {
    return Result::Failure;  // no settled exchange, nothing to bind to
  }
  if (!out || out_len == 0 || out_len > kMaxExportLength ||
      label.empty() || label.size() > kMaxExportLabelLength) {
    return Result::InvalidArgument;
  }
  // the application's labels live under a prefix of their own, so one can never
  // collide with a label znet derives with
  static const char kLabelPrefix[] = "znet exporter label v1:";
  std::vector<unsigned char> info(kLabelPrefix,
                                  kLabelPrefix + sizeof(kLabelPrefix) - 1);
  info.insert(info.end(), label.begin(), label.end());
  return Hkdf(exporter_secret_, sizeof(exporter_secret_), info.data(),
              info.size(), out, out_len)
             ? Result::Success
             : Result::Failure;
}

uint64_t& EncryptionLayer::TxCounter(uint8_t stream) {
  if (stream >= tx_counters_.size()) {
    tx_counters_.resize(static_cast<size_t>(stream) + 1, 0);
  }
  return tx_counters_[stream];
}

ReplayWindow& EncryptionLayer::RxWindow(uint8_t stream) {
  if (stream >= rx_replay_.size()) {
    rx_replay_.resize(static_cast<size_t>(stream) + 1);
  }
  return rx_replay_[stream];
}

std::shared_ptr<Buffer> EncryptionLayer::HandleDecrypt(
    std::shared_ptr<Buffer> buffer) {
  auto mode = buffer->ReadInt<uint8_t>();
  if (mode == kModePlaintext) {
    // Once keys exist the session is encrypted, and a plaintext message is an
    // attacker stripping the encryption rather than a peer being helpful.
    if (key_filled_) {
      ZNET_LOG_ERROR(
          "Plaintext message on an encrypted session, dropping (downgrade "
          "attempt or a peer that lost its keys).");
      return nullptr;
    }
    return buffer;  // handshake, before the mode is settled
  }
  if (mode == kModeAesCbc) {
    // Retired in favour of GCM. Still accepting it would hand an attacker an
    // unauthenticated cipher to downgrade to, so it is refused outright.
    ZNET_LOG_ERROR(
        "Peer used the retired unauthenticated AES-CBC mode; it is probably an "
        "older znet. Dropping.");
    return nullptr;
  }
  if (mode != kModeAesGcm) {
    ZNET_LOG_ERROR("Encryption mode {} is not known/supported!", mode);
    return nullptr;
  }
  if (!key_filled_) {
    ZNET_LOG_ERROR("Encrypted message before the key exchange finished, dropping.");
    return nullptr;
  }
  // Read() only sets an error flag when short, leaving the destination
  // indeterminate, so the length is checked before anything is copied out.
  if (buffer->readable_bytes() < kHeaderLen + kTagLen) {
    ZNET_LOG_ERROR("Encrypted message is too short to hold a nonce and tag, dropping.");
    return nullptr;
  }
  unsigned char header[kHeaderLen];
  buffer->Read(header, sizeof(header));
  const uint8_t stream = header[0];
  const uint64_t counter = ReadCounter(header + 1);

  const size_t remaining = buffer->readable_bytes();
  const auto cipher_len = static_cast<int>(remaining - kTagLen);
  const auto* body = reinterpret_cast<const unsigned char*>(
      buffer->data() + buffer->read_cursor());
  const unsigned char* tag = body + cipher_len;

  unsigned char nonce[kNonceLen];
  BuildNonce(rx_salt_, stream, counter, nonce);

  std::vector<unsigned char> actual(static_cast<size_t>(cipher_len));
  if (!dec_ctx_) {
    dec_ctx_ = EVP_CIPHER_CTX_new();
    dec_keyed_ = false;
  }
  const unsigned char aad[1] = {kModeAesGcm};
  const bool set_dec_key = !dec_keyed_;
  int actual_len =
      DecryptData(dec_ctx_, set_dec_key, rx_key_, nonce, aad,
                  static_cast<int>(sizeof(aad)), body, cipher_len, tag,
                  actual.data());
  if (actual_len >= 0) {
    dec_keyed_ = true;
  }
  buffer->SkipRead(remaining);
  if (actual_len < 0) {
    ZNET_LOG_ERROR(
        "Message failed authentication (stream {}, counter {}), dropping: it "
        "was forged or altered in transit.",
        stream, counter);
    return nullptr;
  }
  // only now, with the tag verified, may the counter move the window
  if (!RxWindow(stream).Accept(counter)) {
    ZNET_LOG_ERROR("Replayed or too-old message (stream {}, counter {}), dropping.",
                   stream, counter);
    return nullptr;
  }
  return std::make_shared<Buffer>(reinterpret_cast<char*>(actual.data()),
                                  static_cast<size_t>(actual_len));
}

std::shared_ptr<Buffer> EncryptionLayer::HandleIn(
    std::shared_ptr<Buffer> buffer) {
  return HandleDecrypt(buffer);
}

std::shared_ptr<Buffer> EncryptionLayer::HandleOut(
    std::shared_ptr<Buffer> buffer, uint8_t stream) {
  // the message starts at the read cursor, not at zero: the send pipeline
  // reserves headroom for the byte prepended below
  if (buffer->readable_bytes() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    ZNET_LOG_ERROR("Buffer length is too large");
    return nullptr;
  }
  int buffer_len = static_cast<int>(buffer->readable_bytes());
  std::shared_ptr<Buffer> new_buffer = std::make_shared<Buffer>();
  if (enable_encryption_) {
    // GCM is a stream cipher: the ciphertext is exactly as long as the input.
    // The output is laid out up front and encrypted straight into place; the
    // scratch ciphertext vector this used to build was a second allocation
    // and a full copy on every message. The two front bytes let the TCP
    // transport frame in place afterwards.
    new_buffer->ReserveHeadroom(2);
    new_buffer->ReserveExact(2 + 1 + kHeaderLen +
                             static_cast<size_t>(buffer_len) + kTagLen);
    new_buffer->WriteInt<uint8_t>(kModeAesGcm);
    const size_t header_pos = new_buffer->write_cursor();
    new_buffer->SkipWrite(kHeaderLen);  // backfilled once the counter is taken
    const size_t ciphertext_pos = new_buffer->write_cursor();
    auto* ciphertext_dst = reinterpret_cast<unsigned char*>(
        new_buffer->data_mutable() + ciphertext_pos);

    unsigned char tag[kTagLen];
    unsigned char nonce[kNonceLen];
    const unsigned char aad[1] = {kModeAesGcm};
    uint64_t counter;
    int ciphertext_len;
    {
      std::lock_guard<std::mutex> lock(enc_mutex_);
      uint64_t& next = TxCounter(stream);
      if (next == kMaxCounter) {
        // Wrapping would repeat a (key, nonce) pair, which breaks GCM
        // completely. Unreachable in practice at 2^56 messages on one stream,
        // but the consequence is bad enough to check rather than assume.
        ZNET_LOG_ERROR("Nonce counter exhausted on stream {}, refusing to send.",
                       stream);
        return nullptr;
      }
      counter = next++;
      BuildNonce(tx_salt_, stream, counter, nonce);
      if (!enc_ctx_) {
        enc_ctx_ = EVP_CIPHER_CTX_new();
        cipher_keyed_ = false;
      }
      const bool set_key = !cipher_keyed_;
      ciphertext_len =
          EncryptData(enc_ctx_, set_key, tx_key_, nonce, aad,
                      static_cast<int>(sizeof(aad)),
                      reinterpret_cast<const unsigned char*>(
                          buffer->read_cursor_data()),
                      buffer_len, ciphertext_dst, tag);
      if (ciphertext_len >= 0) {
        cipher_keyed_ = true;
      }
    }
    if (ciphertext_len < 0) {
      ZNET_LOG_ERROR("Encryption failed, dropping message.");
      return nullptr;
    }

    new_buffer->SkipWrite(static_cast<size_t>(ciphertext_len));
    new_buffer->Write(tag, sizeof(tag));

    unsigned char header[kHeaderLen];
    header[0] = stream;
    WriteCounter(header + 1, counter);
    const size_t end = new_buffer->write_cursor();
    new_buffer->set_write_cursor(header_pos);
    new_buffer->Write(header, sizeof(header));
    new_buffer->set_write_cursor(end);
    return new_buffer;
  }
  // in place when there is headroom left, otherwise a fresh buffer
  if (buffer->PrependInt8(0)) {  // no encryption
    return buffer;
  }
  new_buffer->ReserveHeadroom(2);  // room for the transport's frame
  new_buffer->ReserveExact(static_cast<size_t>(buffer_len) + 3);
  new_buffer->WriteInt<uint8_t>(0);  // no encryption
  new_buffer->Write(buffer->read_cursor_data(), static_cast<size_t>(buffer_len));
  return new_buffer;
}

void EncryptionLayer::OnHandshakePacket(
    std::shared_ptr<HandshakePacket> packet) {
  if (peer_pkey_ || key_filled_) {
    ZNET_LOG_ERROR("Received handshake packet twice, closing the connection!");
    session_.Close();
    return;
  }
  if (session_.is_initiator()) {
    // the server states the parameters outright; adopt them.
    session_.SetNegotiatedCompression(
        static_cast<CompressionType>(packet->compression_));
    if (!packet->encryption_) {
      negotiated_ = true;
      ZNET_LOG_DEBUG("Server selected an unencrypted session.");
      if (!sent_ready_) {
        SendReady();
      }
      return;
    }
    if (!packet->pub_key_) {
      ZNET_LOG_ERROR(
          "Server selected an encrypted session but sent no public key, "
          "closing the connection!");
      session_.Close();
      return;
    }
  } else {
    // accepting side: our own policy decides, the client only supplies a key.
    if (!want_encryption_) {
      negotiated_ = true;
      ZNET_LOG_DEBUG("Server selected an unencrypted session.");
      if (!sent_handshake_) {
        SendHandshake();
      } else if (!sent_ready_) {
        SendReady();
      }
      return;
    }
    if (!packet->pub_key_) {
      ZNET_LOG_ERROR(
          "Client offered no public key but this server requires encryption, "
          "closing the connection!");
      session_.Close();
      return;
    }
  }

  peer_pkey_ = std::move(packet->pub_key_);
  shared_secret_ = ComputeSharedSecret(pub_key_.get(), peer_pkey_.get(),
                                       &shared_secret_len_);
  if (!shared_secret_ || shared_secret_len_ == 0) {
    ZNET_LOG_ERROR("ComputeSharedSecret failed! secret={}, len={}", static_cast<void*>(shared_secret_), shared_secret_len_);
    session_.Close();
    return;
  }
  if (!DeriveDirectionalKeys()) {
    ZNET_LOG_ERROR(
        "Failed to derive key from DH secret, closing the connection!");
    session_.Close();
    return;
  }
  key_filled_ = true;
  negotiated_ = true;
  ZNET_LOG_DEBUG("Handshake key exchange complete, initiator={}", session_.is_initiator());

  if (!sent_handshake_) {
    SendHandshake();
  } else if (!sent_ready_) {
    SendReady();
  }
}

void EncryptionLayer::OnAcknowledgePacket(
    std::shared_ptr<ConnectionReadyPacket> packet) {
  ZNET_LOG_DEBUG("OnAcknowledgePacket: initiator={}", session_.is_initiator());
  if (!negotiated_) {
    ZNET_LOG_ERROR(
        "Received connection complete packet it wasn't expected, closing the "
        "connection!");
    session_.Close();
    return;
  }
  if (packet->magic_ != "343693b5-2b04-4d56-a3b5-48582ca37c7d") {
    ZNET_LOG_ERROR(
        "Received connection complete packet has invalid magic '{}', closing the "
        "connection!", packet->magic_);
    session_.Close();
    return;
  }
  if (!sent_ready_) {
    SendReady();
  }
  session_.SetHandler(nullptr);
  session_.SetCodec(nullptr);
  session_.Ready();
}

void EncryptionLayer::SendHandshake() {
  ZNET_LOG_DEBUG("SendHandshake: initiator={}, want_encryption={}, has_key={}",
                 session_.is_initiator(), want_encryption_,
                 static_cast<bool>(pub_key_));
  auto packet = std::make_shared<HandshakePacket>();
  // the initiator always offers a key so the server may pick either mode. The
  // server states its decision and includes a key only when it encrypts.
  bool offer_key = session_.is_initiator() || want_encryption_;
  if (offer_key && pub_key_) {
    packet->pub_key_ = CloneKey(pub_key_);
  }
  packet->encryption_ = want_encryption_;
  packet->compression_ =
      GetCompressionTypeRaw(session_.negotiated_compression());
  session_.SendImmediate(packet);
  sent_handshake_ = true;
}

void EncryptionLayer::SendReady() {
  // the negotiated outcome, not what this side asked for
  enable_encryption_ = key_filled_;
  auto packet = std::make_shared<ConnectionReadyPacket>();
  packet->magic_ = "343693b5-2b04-4d56-a3b5-48582ca37c7d";
  session_.SendImmediate(packet);
  sent_ready_ = true;
}

}  // namespace znet