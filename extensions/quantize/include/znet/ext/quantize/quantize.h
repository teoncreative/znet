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
// umbrella header for the znet quantisation core.
//
//   #include "znet/ext/quantize/quantize.h"
//
// plain floats in, integers out. No Buffer, no vector library, no engine
// types. znet-glm, znet-bitpack and the physics adapters all sit on top of
// this so that a value written through one comes back correctly through
// another.
//

#pragma once

#include "znet/ext/quantize/direction.h"
#include "znet/ext/quantize/rotation.h"
#include "znet/ext/quantize/scalar.h"
