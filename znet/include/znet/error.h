//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_ERROR_H_
#define ZNET_ERROR_H_

#include "znet/precompiled.h"

namespace znet {

/**
 * @brief The last socket error of the calling thread, formatted as
 *        "code (text)". Read it immediately: any socket call resets it.
 */
std::string GetLastErrorInfo();

}  // namespace znet


#endif  // ZNET_ERROR_H_
