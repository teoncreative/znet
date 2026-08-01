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
// umbrella header for the znet nlohmann-json extension.
//
//   #include "znet/ext/json/json.h"
//
// length-prefixed json in a Buffer, as MessagePack or as text, with the bounds
// that make parsing network-supplied json safe, plus a ready-made
// PacketSerializer.
//

#pragma once

#include "znet/ext/json/codec.h"
#include "znet/ext/json/packet.h"
