
//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"
#include <chrono>

/*
 * By default, this class will use precise sleep; this could result in more
 * CPU usage but much more precise timing. To disable precise sleep, enable
 * ZNET_PREFER_STD_SLEEP.
 */
class Scheduler {
 public:
  using Clock = std::chrono::high_resolution_clock;
  using TimePoint = std::chrono::time_point<Clock>;
  using Duration = std::chrono::microseconds;

  Scheduler(int tps);
  ~Scheduler();

  void SetTicksPerSecond(int tps);

  void Start();
  void End();

  void Wait();

  static void PreciseSleep(Duration duration);
 private:

  TimePoint start_time_;
  TimePoint end_time_;
  Duration delta_time_;
  Duration target_delta_time_;
  int tps_;
};