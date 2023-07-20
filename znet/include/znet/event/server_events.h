
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

#include "event.h"
#include "../base/server_session.h"

namespace znet {

  class ClientConnectedEvent : public Event {
  public:
    ClientConnectedEvent(Ref<ServerSession> session) : session_(session) {}

    Ref<ServerSession> session() { return session_; }

    ZNET_EVENT_CLASS_TYPE(ClientConnected)
    ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
  private:
    Ref<ServerSession> session_;
  };

}