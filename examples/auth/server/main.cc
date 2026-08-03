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
// The server side of session-bound authentication. See common/packets.h.
//
// It is configured with one public key, the service's. It stores nothing per
// user: a client's key arrives inside the token, and the service's signature is
// what makes it worth trusting.
//

#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/server.h"
#include "znet/server_events.h"
#include "znet/signal_handler.h"

#include "packets.h"
#include "service.h"  // only for ServicePublicKey(); see the note there

using namespace znet;

// What an authenticated client may do. Installed only after the proof checks
// out, so an unauthenticated peer cannot reach any of it.
class GameHandler : public PacketHandler<GameHandler, ChatPacket> {
 public:
  GameHandler(std::shared_ptr<PeerSession> session, std::string user_id)
      : session_(std::move(session)), user_id_(std::move(user_id)) {}

  void OnPacket(std::shared_ptr<ChatPacket> packet) {
    ZNET_LOG_INFO("<{}> {}", user_id_, packet->text);
    auto reply = std::make_shared<ChatPacket>();
    reply->text = "echo: " + packet->text;
    session_->SendPacket(reply);
  }

 private:
  std::shared_ptr<PeerSession> session_;
  std::string user_id_;
};

// The only thing a fresh connection is allowed to send.
class LoginHandler : public PacketHandler<LoginHandler, LoginPacket> {
 public:
  explicit LoginHandler(std::shared_ptr<PeerSession> session)
      : session_(std::move(session)) {}

  void OnPacket(std::shared_ptr<LoginPacket> packet) {
    std::string detail;
    if (!Authenticate(*packet, &detail)) {
      ZNET_LOG_WARN("Rejected login from {}: {}",
                    session_->remote_address()->readable(), detail);
      Reply(false, detail);
      // give the reply a tick to leave before the socket goes
      session_->Process();
      session_->Close();
      return;
    }

    ZNET_LOG_INFO("Authenticated {} from {}", packet->token.user_id,
                  session_->remote_address()->readable());
    Reply(true, "welcome");
    // swapping the handler is what opens the rest of the protocol up
    session_->SetHandler(
        std::make_shared<GameHandler>(session_, packet->token.user_id));
  }

 private:
  bool Authenticate(const LoginPacket& packet, std::string* detail) {
    // 1. Is the token genuine and current? This is the only place the service's
    //    key is used, and the only key this server is configured with.
    if (!packet.token.VerifiedBy(ServicePublicKey())) {
      *detail = "token is not signed by the service, or has expired";
      return false;
    }

    // 2. What does *this* session look like? The client signed its own copy of
    //    these bytes. Nobody outside the session can compute them.
    unsigned char expected[kExportLength];
    if (!session_->ExportKeyingMaterial(kAuthExportLabel, expected,
                                        sizeof(expected))) {
      *detail = "session has no key exchange to bind to";  // encryption is off
      return false;
    }

    // 3. Did the holder of the key the token names sign those bytes?
    //
    //    This is the step a relayed token cannot pass. An interceptor holds a
    //    different session with us than the one the client signed for, so the
    //    bytes it would have to match are not the bytes it has, and it cannot
    //    produce the signature without the client's private key.
    std::vector<unsigned char> message(expected, expected + sizeof(expected));
    if (!Ed25519Verify(packet.token.client_public_key, message, packet.proof)) {
      *detail = "proof does not match this session";
      return false;
    }
    return true;
  }

  void Reply(bool accepted, const std::string& detail) {
    auto result = std::make_shared<LoginResultPacket>();
    result->accepted = accepted;
    result->detail = detail;
    session_->SendPacket(result);
  }

  std::shared_ptr<PeerSession> session_;
};

bool OnNewSession(IncomingClientConnectedEvent& event) {
  PeerSession& session = *event.session();
  session.SetCodec(MakeAuthCodec());
  // every connection starts unauthenticated
  session.SetHandler(std::make_shared<LoginHandler>(event.session()));
  return false;
}

void OnEvent(Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<IncomingClientConnectedEvent>(
      ZNET_BIND_GLOBAL_FN(OnNewSession));
}

int main() {
  Result result;
  if ((result = znet::Init()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to initialize znet: {}", GetResultString(result));
    return 1;
  }

  ServerConfig config{"127.0.0.1", 25000, std::chrono::seconds(10)};
  // the export exists only when the session is encrypted, which is the default
  config.child_options.common.encryption = true;

  Server server{config};
  RegisterSignalHandler(
      [&server](Signal) -> bool {
        server.Stop();
        return server.shutdown_complete();
      },
      znet::kSignalInterrupt);
  server.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if ((result = server.Bind()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to bind: {}", GetResultString(result));
    return 1;
  }
  if ((result = server.Listen()) != Result::Success) {
    ZNET_LOG_ERROR("Failed to listen: {}", GetResultString(result));
    return 1;
  }

  server.Wait();
  znet::Cleanup();
  return 0;
}
