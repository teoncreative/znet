
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
#include "znet/base/event.h"

namespace znet {

/**
 * @class ClientConnectedToServerEvent
 * @brief Represents an event triggered when a client successfully connects to the server.
 *
 * @details
 * This event is where you would setup the peer, set the codec, handlers and
 * the user pointer if needed.
 *
 */
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

/**
 * @class ClientDisconnectedFromServerEvent
 * @brief Represents an event triggered when a client disconnects from the server.
 *
 * This event encapsulates the session details of the disconnected client.
 */
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