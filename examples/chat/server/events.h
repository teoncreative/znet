
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

#include "znet/znet.h"

using namespace znet;
class User;

class UserAuthorizedEvent : public Event {
 public:
  UserAuthorizedEvent(User& user) : user_(user) {}

  User& user() { return user_; }

  ZNET_EVENT_CLASS_TYPE(UserAuthorized)
  ZNET_EVENT_CLASS_CATEGORY(EventCategoryServer)
 private:
  User& user_;
};