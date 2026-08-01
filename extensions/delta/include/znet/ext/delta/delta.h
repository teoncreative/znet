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
// umbrella header for the znet delta compression extension.
//
//   #include "znet/ext/delta/delta.h"
//
// three pieces that fit together: SnapshotHistory decides which baseline the
// peer actually has, DeltaWriter sends only what differs from it, and
// DeltaReader puts the snapshot back together.
//

#pragma once

#include "znet/ext/delta/history.h"
#include "znet/ext/delta/reader.h"
#include "znet/ext/delta/sequence.h"
#include "znet/ext/delta/writer.h"
