
//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "logger.h"

#define MATCH_AND_DO(a, b, do, fmt)  \
  if (a != b) {                    \
    ZNET_LOG_ERROR(fmt, a, b, __LINE__); \
    do;                       \
  }
#define MATCH_AND_EXIT(a, b, fmt) MATCH_AND_DO(a, b, exit(1), fmt)
#define MATCH_AND_EXIT_A(a, b) MATCH_AND_EXIT(a, b, "{}(a) and {}(b) did not mach at line {}")
#define MATCH_AND_RETURN(a, b, fmt) MATCH_AND_DO(a, b, return, fmt)
#define MATCH_AND_RETURN_A(a, b) MATCH_AND_RETURN(a, b, "{}(a) and {}(b) did not mach at line {}")
