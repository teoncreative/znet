
//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_SCHEDULER_H_
#define ZNET_SCHEDULER_H_

#include "znet/precompiled.h"
#include <chrono>

#ifndef ZNET_PREFER_STD_SLEEP
#define ZNET_PREFER_STD_SLEEP 0
#endif

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

  Scheduler(uint16_t tps);
  ~Scheduler();

  void SetTicksPerSecond(uint16_t tps);

  void Start();
  void End();

  void Wait();

  /**
   * @brief How much of the tick is left after the last Start()/End() pair.
   *
   * Callers that can be woken early wait on this themselves rather than
   * calling Wait().
   */
  ZNET_NODISCARD Duration remaining() const {
    return delta_time_ < target_delta_time_ ? target_delta_time_ - delta_time_
                                            : Duration::zero();
  }

  static void PreciseSleep(Duration duration);
 private:

  TimePoint start_time_;
  TimePoint end_time_;
  Duration delta_time_;
  Duration target_delta_time_;
  uint16_t tps_{};
};

#endif  // ZNET_SCHEDULER_H_
