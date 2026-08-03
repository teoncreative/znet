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
// The client side of session-bound authentication. See common/packets.h.
//
// Two steps that are easy to conflate: getting a token from the service, which
// happens once and has nothing to do with any connection, and proving the token
// is yours *on this connection*, which happens per connection and is what the
// export is for.
//

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"

#include "packets.h"
#include "service.h"

using namespace znet;

namespace {

// The client's own identity key. A real client generates this once and keeps it
// in the OS keystore; it never leaves the machine, which is what makes a stolen
// token useless on its own.
unsigned char g_public_key[kEd25519KeyLength];
unsigned char g_private_key[kEd25519KeyLength];
AuthToken g_token;

}  // namespace

class ClientHandler : public PacketHandler<ClientHandler, LoginResultPacket, ChatPacket> {
 public:
  explicit ClientHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(std::shared_ptr<LoginResultPacket> packet) {
    if (!packet->accepted) {
      ZNET_LOG_ERROR("Login refused: {}", packet->detail);
      session_->Close();
      return;
    }
    ZNET_LOG_INFO("Login accepted: {}", packet->detail);
    auto chat = std::make_shared<ChatPacket>();
    chat->text = "hello, authenticated world";
    session_->SendPacket(chat);
  }

  void OnPacket(std::shared_ptr<ChatPacket> packet) {
    ZNET_LOG_INFO("server: {}", packet->text);
  }

 private:
  std::shared_ptr<PeerSession> session_;
};

bool OnConnect(ClientConnectedToServerEvent& event) {
  PeerSession& session = *event.session();
  session.SetCodec(MakeAuthCodec());
  session.SetHandler(std::make_shared<ClientHandler>(event.session()));

  // Per connection: what does *this* session look like? The server derives the
  // same 32 bytes and nobody else can, so a signature over them proves the
  // token is being presented on the session it was signed for.
  unsigned char binding[kExportLength];
  if (!session.ExportKeyingMaterial(kAuthExportLabel, binding, sizeof(binding))) {
    // unencrypted session: there is no exchange to bind to, so refuse rather
    // than fall back to sending a token that could be relayed
    ZNET_LOG_ERROR("No keying material to bind to, refusing to send the token.");
    session.Close();
    return false;
  }

  auto login = std::make_shared<LoginPacket>();
  // We send the same token to the other side
  login->token = g_token;
  std::vector<unsigned char> message(binding, binding + sizeof(binding));
  // Then we generate the proof using our private key and the binding label.
  if (!Ed25519Sign(g_private_key, message, login->proof)) {
    ZNET_LOG_ERROR("Failed to sign the session binding.");
    session.Close();
    return false;
  }

  // The binding itself never goes on the wire, only the signature over it.
  // Sending it would hand a listener everything needed to forge this proof.
  session.SendPacket(login);
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnect));
}

int main() {
  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  // Once, before any connection: make an identity and have the service vouch
  // for it. Over a real network this is an HTTPS call that logs the user in.
  if (!GenerateEd25519(g_public_key, g_private_key)) {
    ZNET_LOG_ERROR("Failed to generate a client key.");
    return 1;
  }
  // Hand off the user id and public key, and get a token from the service
  if (!RequestToken("player-one", g_public_key, &g_token)) {
    ZNET_LOG_ERROR("The service refused to issue a token.");
    return 1;
  }
  ZNET_LOG_INFO("Got a token for {}, valid for {} seconds.", g_token.user_id,
                g_token.expires_at - NowSeconds());

  ClientConfig config{"127.0.0.1", 25000, std::chrono::seconds(10)};
  Client client{config};
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = client.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }
  if ((result = client.Connect()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to connect: {}", GetResultString(result));
    return 1;
  }

  client.Wait();
  znet::Cleanup();
  return 0;
}
