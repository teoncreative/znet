
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

#include "../base/client_session.h"
#include "../base/server_session.h"
#include "event.h"

namespace znet {

class ServerClientConnectedEvent : public Event {
 public:
  ServerClientConnectedEvent(Ref<ServerSession> session) : session_(session) {}

  Ref<ServerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ServerClientConnected)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Ref<ServerSession> session_;
};

class ServerClientDisconnectedEvent : public Event {
 public:
  ServerClientDisconnectedEvent(Ref<ServerSession> session)
      : session_(session) {}

  Ref<ServerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ServerClientDisconnected)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  Ref<ServerSession> session_;
};

class ClientConnectedToServerEvent : public Event {
 public:
  ClientConnectedToServerEvent(Ref<ClientSession> session)
      : session_(session) {}

  Ref<ClientSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ClientConnectedToServer)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryClient)
 private:
  Ref<ClientSession> session_;
};
}  // namespace znet