
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
#include "znet/peer_session.h"
#include "znet/event/event.h"

namespace znet {

class ClientConnectedToServerEvent : public Event {
 public:
  explicit ClientConnectedToServerEvent(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  std::shared_ptr<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ClientConnectedToServerEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryClient)
 private:
  std::shared_ptr<PeerSession> session_;
};

class ClientDisconnectedFromServerEvent : public Event {
 public:
  explicit ClientDisconnectedFromServerEvent(std::shared_ptr<PeerSession> session)
      : session_(session) {}

  std::shared_ptr<PeerSession> session() { return session_; }

  ZNET_EVENT_CLASS_TYPE(ClientDisconnectedFromServerEvent)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryClient)
 private:
  std::shared_ptr<PeerSession> session_;
};

}  // namespace znet