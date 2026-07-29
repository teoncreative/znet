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

bool GenerateIV(unsigned char* iv, int iv_length) {
  if (RAND_bytes(iv, iv_length) != 1) {
    return false;
  }
  return true;
}

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

bool DeriveKeyFromSharedSecret(const unsigned char* shared_secret,
                               size_t secret_len, unsigned char* key,
                               size_t key_len) {
  if (secret_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      key_len > static_cast<size_t>(std::numeric_limits<int>::max())) {
    ZNET_LOG_ERROR("Secret or key length too large");
    return false;
  }

  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!pctx) {
    ZNET_LOG_ERROR("Failed to create EVP_PKEY_CTX for HKDF.");
    return false;
  }

  size_t out_len = key_len;  // EVP_PKEY_derive needs size_t*
  if (EVP_PKEY_derive_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(pctx, reinterpret_cast<const unsigned char*>("salt"), 4) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(pctx, shared_secret,
                                 static_cast<int>(secret_len)) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(pctx, reinterpret_cast<const unsigned char*>("info"), 4) <= 0 ||
      EVP_PKEY_derive(pctx, key, &out_len) <= 0) {  // THIS WAS MISSING!
    ZNET_LOG_ERROR("Failed to derive key using HKDF.");
    EVP_PKEY_CTX_free(pctx);
    return false;
  }

  EVP_PKEY_CTX_free(pctx);
  return true;
}

// `ctx` is owned by the caller and reused across messages. `set_key` is true
// only the first time, so the AES key schedule is derived once per session
// instead of once per message; later calls reset the IV and nothing else.
int EncryptData(EVP_CIPHER_CTX* ctx, bool set_key,
                const unsigned char* plaintext, int plaintext_len,
                const unsigned char* key, const unsigned char* iv,
                unsigned char* ciphertext) {
  if (!ctx) {
    ZNET_LOG_ERROR("Failed to create EVP_CIPHER_CTX.");
    return 0;
  }

  const int ok = set_key
                     ? EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv)
                     : EVP_EncryptInit_ex(ctx, nullptr, nullptr, nullptr, iv);
  if (1 != ok) {
    ZNET_LOG_ERROR("Failed to initialize encryption.");
    return 0;
  }

  int ciphertext_len = 0;
  if (1 != EVP_EncryptUpdate(ctx, ciphertext, &ciphertext_len, plaintext,
                             plaintext_len)) {
    ZNET_LOG_ERROR("Failed to encrypt data.");
    return 0;
  }

  int len;
  if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len)) {
    ZNET_LOG_ERROR("Failed to finalize encryption.");
    return 0;
  }
  ciphertext_len += len;
  return ciphertext_len;
}

// reuses `ctx` the same way EncryptData does; see the note there.
int DecryptData(EVP_CIPHER_CTX* ctx, bool set_key,
                const unsigned char* ciphertext, int ciphertext_len,
                unsigned char* key, unsigned char* iv,
                unsigned char* plaintext) {
  if (!ctx) {
    ZNET_LOG_ERROR("Failed to create EVP_CIPHER_CTX");
    return 0;
  }

  const int ok = set_key
                     ? EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv)
                     : EVP_DecryptInit_ex(ctx, nullptr, nullptr, nullptr, iv);
  if (1 != ok) {
    ZNET_LOG_ERROR("Failed to initialize decryption");
    return 0;
  }

  int plaintext_len = 0;
  if (1 != EVP_DecryptUpdate(ctx, plaintext, &plaintext_len, ciphertext,
                             ciphertext_len)) {
    ZNET_LOG_ERROR("Failed to decrypt data");
    return 0;
  }

  int len;
  if (1 != EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len)) {
    ZNET_LOG_ERROR("Failed to finalize decryption");
    return 0;
  }
  plaintext_len += len;
  return plaintext_len;
}

int CalculateCipherTextLength(int plaintext_len) {
  int block_size = 16;  // Block size in bytes (AES)
  int iv_size = 16;     // IV size in bytes (16 for CBC, 12 for GCM, etc.)

  int ciphertext_len =
      plaintext_len + (block_size - (plaintext_len % block_size)) + iv_size;
  return ciphertext_len;
}

EncryptionLayer::EncryptionLayer(PeerSession& session) : session_(session) {
  pub_key_ = GenerateKey();
  if (!pub_key_) {
    ZNET_LOG_ERROR(
        "Failed to generate key for encryption, closing the connection!");
    session_.Close();
    return;
  }

  key_len_ = 32;
  key_ = new unsigned char[key_len_];

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
  // both of these were leaked once per session. they are key material, so they
  // are wiped rather than just released: a plain delete[] leaves the session
  // key sitting in freed heap for whatever allocates that block next.
  if (key_) {
    OPENSSL_cleanse(key_, key_len_);
    delete[] key_;
    key_ = nullptr;
  }
  if (shared_secret_) {
    OPENSSL_cleanse(shared_secret_, shared_secret_len_);
    delete[] shared_secret_;
    shared_secret_ = nullptr;
  }
}

std::shared_ptr<Buffer> EncryptionLayer::HandleDecrypt(
    std::shared_ptr<Buffer> buffer) {
  auto mode = buffer->ReadInt<uint8_t>();
  if (mode == 0) {
    return buffer;  // no encryption
  }
  if (mode != 1) {
    ZNET_LOG_ERROR("Encryption mode {} is not known/supported!", mode);
    return nullptr;
  }
  // both of these used to be raw new and never freed, once per inbound message
  unsigned char iv[16];
  if (buffer->readable_bytes() < sizeof(iv)) {
    // Read() would leave iv untouched and only set an error flag, and the
    // indeterminate bytes would go straight to OpenSSL
    ZNET_LOG_ERROR("Encrypted message is too short to hold an IV, dropping.");
    return nullptr;
  }
  buffer->Read(iv, sizeof(iv));
  auto cipher_len = static_cast<int>(buffer->readable_bytes());
  // OpenSSL documents the output buffer as needing a block of slack beyond the
  // input, even though CBC with padding cannot use it from a fresh Init.
  std::vector<unsigned char> actual(static_cast<size_t>(cipher_len) +
                                    EVP_MAX_BLOCK_LENGTH);
  const char* data_ptr = buffer->data() + buffer->read_cursor();
  if (!dec_ctx_) {
    dec_ctx_ = EVP_CIPHER_CTX_new();
    dec_keyed_ = false;
  }
  const bool set_dec_key = !dec_keyed_;
  int actual_len = DecryptData(dec_ctx_, set_dec_key,
                               reinterpret_cast<const unsigned char*>(data_ptr),
                               cipher_len, key_, iv, actual.data());
  if (actual_len > 0) {
    dec_keyed_ = true;
  }
  buffer->SkipRead(static_cast<size_t>(cipher_len));
  if (actual_len <= 0) {
    ZNET_LOG_ERROR("Decryption produced no output, dropping message.");
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
    std::shared_ptr<Buffer> buffer) {
  if (buffer->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    ZNET_LOG_ERROR("Buffer length is too large");
    return nullptr;
    }
  int buffer_len = static_cast<int>(buffer->size());
  std::shared_ptr<Buffer> new_buffer = std::make_shared<Buffer>();
  if (enable_encryption_) {
    // owned, not raw new: the three buffers here used to be leaked on every
    // single outgoing message, and so was the plaintext of the round-trip
    // check below.
    std::vector<unsigned char> ciphertext(
        static_cast<size_t>(CalculateCipherTextLength(buffer_len)));
    unsigned char iv[16];
    if (!GenerateIV(iv, sizeof(iv))) {
      ZNET_LOG_ERROR("Failed to generate random IV, will use zeros!");
      memset(iv, 0, sizeof(iv));
    }

    int ciphertext_len;
    {
      std::lock_guard<std::mutex> lock(enc_mutex_);
      if (!enc_ctx_) {
        enc_ctx_ = EVP_CIPHER_CTX_new();
        cipher_keyed_ = false;
      }
      const bool set_key = !cipher_keyed_;
      ciphertext_len =
          EncryptData(enc_ctx_, set_key,
                      reinterpret_cast<const unsigned char*>(buffer->data()),
                      buffer_len, key_, iv, ciphertext.data());
      if (ciphertext_len > 0) {
        cipher_keyed_ = true;
      }
    }
    if (ciphertext_len <= 0) {
      ZNET_LOG_ERROR("Encryption produced no output, dropping message.");
      return nullptr;
    }

    new_buffer->ReserveExact(static_cast<size_t>(ciphertext_len) + 2 + 8 + 16 + 8);
    new_buffer->WriteInt<uint8_t>(1);  // encryption enabled
    new_buffer->Write(iv, sizeof(iv));
    new_buffer->Write(ciphertext.data(), static_cast<size_t>(ciphertext_len));
    return new_buffer;
  }
  new_buffer->ReserveExact(static_cast<size_t>(buffer_len) + 2);
  new_buffer->WriteInt<uint8_t>(0);  // no encryption
  new_buffer->Write(buffer->data(), static_cast<size_t>(buffer_len));
  return new_buffer;
}

void EncryptionLayer::OnHandshakePacket(
    std::shared_ptr<HandshakePacket> packet) {
  //ZNET_LOG_DEBUG("OnHandshakePacket: initiator={}, has_peer_key={}, key_filled={}, sent_handshake={}", session_.is_initiator(), (bool)peer_pkey_, key_filled_, sent_handshake_);
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
  if (!DeriveKeyFromSharedSecret(shared_secret_, shared_secret_len_, key_,
                                 key_len_)) {
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
  session_.SendPacket(packet);
  sent_handshake_ = true;
}

void EncryptionLayer::SendReady() {
  // the negotiated outcome, not what this side asked for
  enable_encryption_ = key_filled_;
  auto packet = std::make_shared<ConnectionReadyPacket>();
  packet->magic_ = "343693b5-2b04-4d56-a3b5-48582ca37c7d";
  session_.SendPacket(packet);
  sent_ready_ = true;
}

}  // namespace znet