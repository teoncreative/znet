
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

#include "event/event.h"
#include "peer_session.h"

namespace znet {

class ServerClientConnectedEvent : public Event {
 public:
  explicit ServerClientConnectedEvent(Ref<PeerSession> session)
      : session_(session) {}

  Ref<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ServerClientConnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Ref<PeerSession> session_;
};

class ServerClientDisconnectedEvent : public Event {
 public:
  explicit ServerClientDisconnectedEvent(Ref<PeerSession> session)
      : session_(session) {}

  Ref<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ServerClientDisconnectedEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Ref<PeerSession> session_;
};

class Server;

class ServerStartupEvent : public Event {
 public:
  ServerStartupEvent(Server& server) : server_(server) {}

  Server& server() { return server_; }

  ZNET_EVENT_CLASS_TYPE(ServerStartupEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Server& server_;
};

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