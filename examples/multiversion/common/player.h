//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 02/08/2025.
//

#ifndef ZNET_PARENT_PLAYER_H
#define ZNET_PARENT_PLAYER_H

#include "types.h"

class Player {
 public:
  Player() {}

 public:
  int protocol_ = 0;
  Vec3 pos_;
};


#endif  //ZNET_PARENT_PLAYER_H
