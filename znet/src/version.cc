//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/version.h"

namespace znet {

// Compiled into the library, so these report the version of the binary rather
// than of whatever headers the caller happens to be building against.
int VersionNumber() {
  return ZNET_VERSION;
}

const char* VersionString() {
  return ZNET_VERSION_STRING;
}

}  // namespace znet
