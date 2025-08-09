//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/base/util.h"

#include <string>
#include <vector>
#include <random>

namespace znet {

std::string GeneratePeerName() {
  static const std::vector<std::string> first = {
      "Skippy",  "Crimson", "Neon",     "Rusty",   "Silent",  "Quantum",
      "Velvet",  "Blaze",   "Echo",     "Frost",   "Solar",   "Pixel",
      "Iron",    "Azure",   "Misty",    "Copper",  "Shadow",  "Polar",
      "Turbo",   "Glitch",  "Apex",     "Boulder", "Cascade", "Drift",
      "Ember",   "Gale",    "Halo",     "Jolt",    "Kinetic", "Lunar",
      "Monarch", "Nimbus",  "Obsidian", "Phoenix", "Quasar"};
  static const std::vector<std::string> second = {
      "Toe",     "Fox",    "Bolt",   "Wing",   "Shade",   "Pulse",  "Drift",
      "Knight",  "Flare",  "Shard",  "Vector", "Chaser",  "Spark",  "Raven",
      "Quill",   "Stream", "Bluff",  "Cipher", "Warden",  "Orbit",  "Arc",
      "Bramble", "Crest",  "Dusk",   "Edge",   "Flicker", "Harbor", "Icicle",
      "Jester",  "Kite",   "Ledger", "Nexus",  "Omen",    "Rune",   "Spire"};

  static thread_local std::mt19937_64 gen{std::random_device{}()};
  std::uniform_int_distribution<size_t> d1(0, first.size() - 1);
  std::uniform_int_distribution<size_t> d2(0, second.size() - 1);

  return first[d1(gen)] + second[d2(gen)];
}

}