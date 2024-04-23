
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

#include "peer_session.h"
#include "event/event.h"

namespace znet {

class ClientConnectedToServerEvent : public Event {
 public:
  ClientConnectedToServerEvent(Ref<PeerSession> session)
      : session_(session) {}

  Ref<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ClientConnectedToServer)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryClient)
 private:
  Ref<PeerSession> session_;
};

class ClientDisconnectedFromServerEvent : public Event {
 public:
  ClientDisconnectedFromServerEvent(Ref<PeerSession> session)
      : session_(session) {}

  Ref<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ClientDisconnectedFromServer)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryClient)
 private:
  Ref<PeerSession> session_;
};

}  // namespace znet