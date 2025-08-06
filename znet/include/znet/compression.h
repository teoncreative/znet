//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 06/08/2025.
//

#ifndef ZNET_PARENT_COMPRESSION_H
#define ZNET_PARENT_COMPRESSION_H

#include "znet/precompiled.h"
#include "znet/buffer.h"

namespace znet {

using CompressionTypeRaw = uint8_t;

enum class CompressionType {
  None,
  Zstandard,
};

CompressionTypeRaw GetCompressionTypeRaw(CompressionType type);
std::string GetCompressionTypeName(CompressionType type);

namespace compr {

std::shared_ptr<Buffer> HandleOutWithType(CompressionType type, std::shared_ptr<Buffer> buffer);
std::shared_ptr<Buffer> HandleInWithType(CompressionType type, std::shared_ptr<Buffer> buffer);
std::shared_ptr<Buffer> HandleInDynamic(std::shared_ptr<Buffer> buffer);

}

}
#endif  //ZNET_PARENT_COMPRESSION_H
