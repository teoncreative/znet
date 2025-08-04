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

#ifndef ZNET_PARENT_TYPES_H
#define ZNET_PARENT_TYPES_H

struct Vec3 {
  double x, y, z;

  Vec3 operator+(Vec3 other) {
    return {x + other.x,
      y + other.y,
      z + other.z};
  }

  Vec3& operator+=(Vec3 other) {
    x += other.x;
    y += other.y;
    z += other.z;
    return *this;
  }

  std::string to_string() const {
    return fmt::format("x: {}, y: {}, z: {}", x, y, z);
  }
};

#endif  //ZNET_PARENT_TYPES_H
