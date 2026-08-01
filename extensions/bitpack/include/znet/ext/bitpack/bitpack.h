//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// umbrella header for the znet bit packing extension.
//
//   #include "znet/ext/bitpack/bitpack.h"
//
// bitWriter and BitReader put sub-byte fields into an ordinary Buffer, so a
// bool costs one bit instead of eight and a value bounded by 1000 costs ten
// instead of sixteen.
//

#pragma once

#include "znet/ext/bitpack/bits.h"
#include "znet/ext/bitpack/reader.h"
#include "znet/ext/bitpack/writer.h"
