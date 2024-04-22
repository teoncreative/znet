//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "encryption.h"
#include "connection_session.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>

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
  *len = i2d_PUBKEY(pkey, &der);  // Serialize the public key to DER format
  if (*len <= 0) {
    fprintf(stderr, "Failed to serialize public key\n");
    if (der){
      OPENSSL_free(der);
    }
    return nullptr;
  }

  return der;  // The caller must free this memory using OPENSSL_free
}

EVP_PKEY* DeserializePublicKey(const unsigned char* der, int len) {
  if (!der || len <= 0) {
    return nullptr;
  }
  const unsigned char* p = der;
  EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &p, len);  // Deserialize the DER data
  if (!pkey) {
    fprintf(stderr, "Failed to deserialize public key\n");
    return nullptr;
  }

  return pkey;
}

EVP_PKEY* GenerateKey() {
  /* Create the context for generating the parameters */
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_DH, NULL);
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
  if (params == NULL){
    EVP_PKEY_CTX_free(pctx);
    return nullptr;
  }

  if (EVP_PKEY_assign(params, EVP_PKEY_DHX, DH_get_2048_256())  <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return nullptr;
  }

  EVP_PKEY_CTX_free(pctx);

  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new(params, NULL);
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
  return dhkey;
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
    std::cerr << "Invalid input to derive_shared_secret." << std::endl;
    return nullptr;
  }

  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, NULL);
  if (!ctx) {
    std::cerr << "Failed to create EVP_PKEY_CTX." << std::endl;
    return nullptr;
  }

  if (EVP_PKEY_derive_init(ctx) <= 0) {
    std::cerr << "Failed to initialize derive context." << std::endl;
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  if (EVP_PKEY_derive_set_peer(ctx, peer_pkey) <= 0) {
    std::cerr << "Failed to set peer key." << std::endl;
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  // Determine buffer length
  if (EVP_PKEY_derive(ctx, nullptr, secret_len) <= 0) {
    std::cerr << "Failed to determine shared secret length." << std::endl;
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  unsigned char* secret = new unsigned char[*secret_len];
  if (!secret) {
    std::cerr << "Failed to allocate memory for shared secret." << std::endl;
    EVP_PKEY_CTX_free(ctx);
    return nullptr;
  }

  // Derive the shared secret
  if (EVP_PKEY_derive(ctx, secret, secret_len) <= 0) {
    std::cerr << "Failed to derive shared secret." << std::endl;
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
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
  if (!pctx) {
    std::cerr << "Failed to create EVP_PKEY_CTX for HKDF." << std::endl;
    return false;
  }

  if (EVP_PKEY_derive_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(pctx, (unsigned char*)"salt", 4) <=
          0 ||  // Example: Salt can be optional
      EVP_PKEY_CTX_set1_hkdf_key(pctx, shared_secret, secret_len) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(pctx, (unsigned char*)"info", 4) <=
          0 ||  // Optional context info
      EVP_PKEY_derive(pctx, key, &key_len) <= 0) {
    std::cerr << "Failed to derive key using HKDF." << std::endl;
    EVP_PKEY_CTX_free(pctx);
    return false;
  }

  EVP_PKEY_CTX_free(pctx);
  return true;
}

int EncryptData(const unsigned char* plaintext, int plaintext_len,
                const unsigned char* key, const unsigned char* iv,
                unsigned char* ciphertext) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    std::cerr << "Failed to create EVP_CIPHER_CTX" << std::endl;
    return false;
  }

  if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv)) {
    std::cerr << "Failed to initialize encryption" << std::endl;
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  int ciphertext_len = 0;
  if (1 != EVP_EncryptUpdate(ctx, ciphertext, &ciphertext_len, plaintext,
                             plaintext_len)) {
    std::cerr << "Failed to encrypt data" << std::endl;
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  int len;
  if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len)) {
    std::cerr << "Failed to finalize encryption" << std::endl;
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  ciphertext_len += len;

  EVP_CIPHER_CTX_free(ctx);
  return ciphertext_len;
}

int DecryptData(const unsigned char* ciphertext, int ciphertext_len,
                unsigned char* key, unsigned char* iv,
                unsigned char* plaintext) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    std::cerr << "Failed to create EVP_CIPHER_CTX" << std::endl;
    return false;
  }

  if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv)) {
    std::cerr << "Failed to initialize decryption" << std::endl;
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  int plaintext_len = 0;
  if (1 != EVP_DecryptUpdate(ctx, plaintext, &plaintext_len, ciphertext,
                             ciphertext_len)) {
    std::cerr << "Failed to decrypt data" << std::endl;
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  int len;
  if (1 != EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len, &len)) {
    std::cerr << "Failed to finalize decryption" << std::endl;
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  plaintext_len += len;

  EVP_CIPHER_CTX_free(ctx);
  return plaintext_len;
}

int CalculateCipherTextLength(int plaintext_len) {
  // Assume these are set or calculated appropriately
  int block_size = 16;  // Block size in bytes (AES)
  int iv_size = 16;     // IV size in bytes (16 for CBC, 12 for GCM, etc.)
  int auth_tag_size =
      16;  // Tag size in bytes (only for authenticated modes like GCM)

  int ciphertext_len =
      plaintext_len + (block_size - (plaintext_len % block_size)) + iv_size;
  //if (is_authenticated_mode) {
  //  ciphertext_len += auth_tag_size;
  //}
  return ciphertext_len;
}

EncryptionLayer::EncryptionLayer(ConnectionSession& session)
    : session_(session), handler_layer_(session) {
  pub_key_ = GenerateKey();
  if (!pub_key_) {
    ZNET_LOG_ERROR(
        "Failed to generate key for encryption, closing the connection!");
    session_.Close();
    return;
  }

  key_len_ = 32;
  key_ = new unsigned char[key_len_];

  auto handshake_packet_handler =
      CreateRef<PacketHandler<HandshakePacket, HandshakePacketSerializerV1>>();
  handshake_packet_handler->AddReceiveCallback(ZNET_BIND_FN(OnHandshakePacket));
  handler_layer_.AddPacketHandler(handshake_packet_handler);
  auto ack_packet_handler =
      CreateRef<PacketHandler<ConnectionCompletePacket,
                              ConnectionCompletePacketSerializerV1>>();
  ack_packet_handler->AddReceiveCallback(ZNET_BIND_FN(OnAcknowledgePacket));
  handler_layer_.AddPacketHandler(ack_packet_handler);
}

void EncryptionLayer::Initialize(bool send) {
  if (send) {
    SendHandshake();
  }
}

EncryptionLayer::~EncryptionLayer() {
  if (peer_pkey_) {
    EVP_PKEY_free(peer_pkey_);
    peer_pkey_ = nullptr;
  }
  if (pub_key_) {
    EVP_PKEY_free(pub_key_);
    pub_key_ = nullptr;
  }
}

Ref<Buffer> EncryptionLayer::HandleDecrypt(Ref<Buffer> buffer) {
  auto mode = buffer->ReadInt<uint16_t>();
  if (mode == 0) {
    return buffer;  // no encryption
  }
  if (mode != 1) {
    ZNET_LOG_ERROR("Encryption mode {} is not known/supported!", mode);
    return nullptr;
  }
  auto actual_size = buffer->ReadInt<uint64_t>();
  auto* actual = new unsigned char[actual_size];
  auto* iv = new unsigned char[16];
  //memset(iv, 0, 16);
  buffer->Read(iv, 16);
  auto cipher_len = buffer->ReadInt<uint64_t>();
  const char* data_ptr = buffer->data() + buffer->read_cursor();
  DecryptData(reinterpret_cast<const unsigned char*>(data_ptr), cipher_len,
              key_, iv, actual);
  return CreateRef<Buffer>(reinterpret_cast<char*>(actual), actual_size);
}

Ref<Buffer> EncryptionLayer::HandleIn(Ref<znet::Buffer> buffer) {
  buffer = HandleDecrypt(buffer);
  if (!handoff_ && buffer) {
    handler_layer_.HandleIn(buffer);
    return nullptr;
  }
  return buffer;
}

Ref<Buffer> EncryptionLayer::HandleOut(Ref<Buffer> buffer) {
  Ref<Buffer> new_buffer = CreateRef<Buffer>();
  if (enable_encryption_) {
    auto* ciphertext =
        new unsigned char[CalculateCipherTextLength(buffer->size())];
    auto* iv = new unsigned char[16];
    if (!GenerateIV(iv, 16)) {
      ZNET_LOG_ERROR("Failed to generate random IV, will use zeros!");
      memset(iv, 0, 16);
    }

    // Encrypt the plaintext
    int ciphertext_len =
        EncryptData(reinterpret_cast<const unsigned char*>(buffer->data()),
                    buffer->size(), key_, iv, ciphertext);
    new_buffer->ReserveExact(ciphertext_len + 2 + 8 + 8);
    new_buffer->WriteInt<uint16_t>(1);  // encryption enabled
    new_buffer->WriteInt<uint64_t>(buffer->size());
    new_buffer->Write(iv, 16);
    new_buffer->WriteInt<uint64_t>(ciphertext_len);
    new_buffer->Write(ciphertext, ciphertext_len);

    auto* actual = new unsigned char[buffer->size()];
    DecryptData(ciphertext, ciphertext_len, key_, iv, actual);
    return new_buffer;
  }
  new_buffer->ReserveExact(buffer->size() + 2);
  new_buffer->WriteInt<uint16_t>(0);  // no encryption
  new_buffer->Write(buffer->data(), buffer->size());
  return new_buffer;
}

void EncryptionLayer::OnHandshakePacket(ConnectionSession&,
                                        Ref<HandshakePacket> packet) {
  if (peer_pkey_ || key_filled_) {
    ZNET_LOG_ERROR("Received handshake packet twice, closing the connection!");
    session_.Close();
    return;
  }
  peer_pkey_ = packet->pub_key_;
  packet->owner_ = false;  // we own the key now
  shared_secret_ =
      ComputeSharedSecret(pub_key_, peer_pkey_, &shared_secret_len_);
  if (!DeriveKeyFromSharedSecret(shared_secret_, shared_secret_len_, key_,
                                 key_len_)) {
    ZNET_LOG_ERROR(
        "Failed to derive key from DH secret, closing the connection!");
    session_.Close();
    return;
  }
  key_filled_ = true;

  if (!sent_handshake_) {
    SendHandshake();
  } else if (!sent_ack_) {
    SendAcknowledge();
  }
}

void EncryptionLayer::OnAcknowledgePacket(
    ConnectionSession&, Ref<ConnectionCompletePacket> packet) {
  if (!peer_pkey_ || !key_filled_) {
    ZNET_LOG_ERROR(
        "Received connection complete packet it wasn't expected, closing the "
        "connection!");
    session_.Close();
    return;
  }
  if (packet->magic_ != "343693b5-2b04-4d56-a3b5-48582ca37c7d") {
    ZNET_LOG_ERROR(
        "Received connection complete packet has invalid magic, closing the "
        "connection!");
    session_.Close();
    return;
  }
  if (!sent_ack_) {
    SendAcknowledge();
  }
  handoff_ = true;
  session_.Ready();
}

void EncryptionLayer::SendHandshake() {
  auto packet = CreateRef<HandshakePacket>();
  packet->pub_key_ = pub_key_;
  packet->owner_ = false;
  auto buffer = handler_layer_.HandleOut(packet);
  if (buffer) {
    session_.SendRaw(buffer);
  }
  sent_handshake_ = true;
}

void EncryptionLayer::SendAcknowledge() {
  enable_encryption_ = true;
  auto packet = CreateRef<ConnectionCompletePacket>();
  packet->magic_ = "343693b5-2b04-4d56-a3b5-48582ca37c7d";
  auto buffer = handler_layer_.HandleOut(packet);
  if (buffer) {
    session_.SendRaw(buffer);
  }
  sent_ack_ = true;
}

}  // namespace znet