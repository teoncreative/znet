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
// umbrella header for the znet reflection extension.
//
//   #include "znet/ext/reflect/reflect_all.h"
//
// writeAuto/ReadAuto serialize a struct from its fields: deduced for plain
// aggregates, declared with ZNET_REFLECT for anything else. AutoPacket and
// MakeAutoSerializer drop the result into znet's codec.
//

#pragma once

#include "znet/ext/reflect/aggregate.h"
#include "znet/ext/reflect/packet.h"
#include "znet/ext/reflect/reflect.h"
#include "znet/ext/reflect/serialize.h"
