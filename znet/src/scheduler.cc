//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/scheduler.h"
#include <thread>

Scheduler::Scheduler(int tps) {
  SetTicksPerSecond(tps);
}

Scheduler::~Scheduler() {}

void Scheduler::SetTicksPerSecond(int tps) {
  if (tps == tps_) {
    return;
  }
  tps_ = tps;
  target_delta_time_ = std::chrono::duration_cast<Duration>(
      std::chrono::seconds(1) / (float)tps);
}

void Scheduler::Start() {
  start_time_ = Clock::now();
}

void Scheduler::End() {
  end_time_ = Clock::now();
  delta_time_ = std::chrono::duration_cast<Duration>(end_time_ - start_time_);
}

void Scheduler::Wait() {
  if (delta_time_ < target_delta_time_) {
    auto sleep =
        std::chrono::duration_cast<Duration>(target_delta_time_ - delta_time_);
#ifndef ZNET_PREFER_STD_SLEEP
    PreciseSleep(sleep);
#else
    std::this_thread::sleep_for(sleep);
#endif
  }
}

void Scheduler::PreciseSleep(Duration duration) {
  double estimate = 5e-3;
  double mean = 5e-3;
  double m2 = 0;
  double count = 1;
  double observed = 0;
  double delta = 0;
  double stddev = 0;

  // microseconds to seconds
  double seconds = duration.count() / 1000000.0;
  while (seconds > estimate) {
    TimePoint start = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    TimePoint end = Clock::now();

    observed = (end - start).count() / 1000000000.0;
    seconds -= observed;

    count++;
    delta = observed - mean;
    mean += delta / count;
    m2 += delta * (observed - mean);
    stddev = std::sqrt(m2 / (count - 1));
    estimate = mean + stddev;
  }

  // spin lock
  TimePoint start = Clock::now();
  while ((Clock::now() - start).count() / 1000000000.0 < seconds);
}
