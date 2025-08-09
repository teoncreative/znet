
//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"
#include "znet/base/event.h"
#include "znet/peer_session.h"

namespace znet {

/**
 * @brief Event triggered when a client connects to this server.
 *
 * This event is where you would setup the peer, set the codec, handlers and
 * the user pointer if needed.
 */
class IncomingClientConnectedEvent : public Event {
 public:
  explicit IncomingClientConnectedEvent(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  std::shared_ptr<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(IncomingClientConnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  std::shared_ptr<PeerSession> session_;
};

DEPRECATED_TYPE_ALIAS(ServerClientConnectedEvent,
                      IncomingClientConnectedEvent, "Use IncomingClientConnectedEvent instead")

/**
 * @brief Event triggered when a client disconnects from the server.
 */
class ServerClientDisconnectedEvent : public Event {
 public:
  explicit ServerClientDisconnectedEvent(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  std::shared_ptr<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ServerClientDisconnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  std::shared_ptr<PeerSession> session_;
};

class Server;

/**
 * @brief Event triggered when the server starts up.
 */
class ServerStartupEvent : public Event {
 public:
  ServerStartupEvent(Server& server) : server_(server) {}

  Server& server() { return server_; }

  ZNET_EVENT_CLASS_TYPE(ServerStartupEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Server& server_;
};

/**
 * @brief Event triggered when the server shuts down.
 */
class ServerShutdownEvent : public Event {
 public:
  ServerShutdownEvent(Server& server) : server_(server) {}

  Server& server() { return server_; }

  ZNET_EVENT_CLASS_TYPE(ServerShutdownEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Server& server_;
};

}  // namespace znet