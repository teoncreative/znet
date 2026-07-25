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
  /**
   * @brief Resolved at session start to whatever the build supports.
   *
   * Never appears on the wire; see ResolveCompressionType().
   */
  Default = 0xFF,
};

/**
 * @brief Resolves CompressionType::Default to a concrete type for this build.
 *
 * Anything else passes through unchanged, so an explicit Zstandard on a build
 * without zstd still warns at the codec instead of being silently rewritten.
 *
 * @param type The configured compression type.
 * @return Zstandard or None for Default; otherwise `type`.
 */
constexpr CompressionType ResolveCompressionType(CompressionType type) {
  if (type != CompressionType::Default) {
    return type;
  }
#ifdef ZNET_USE_ZSTD
  return CompressionType::Zstandard;
#else
  return CompressionType::None;
#endif
}

constexpr auto operator<=>(CompressionType lhs, CompressionType rhs) noexcept {
  return static_cast<int>(lhs) <=> static_cast<int>(rhs);
}

constexpr bool operator==(CompressionType lhs, CompressionType rhs) noexcept {
  return static_cast<int>(lhs) == static_cast<int>(rhs);
}

CompressionTypeRaw GetCompressionTypeRaw(CompressionType type);
std::string GetCompressionTypeName(CompressionType type);

namespace compr {

std::shared_ptr<Buffer> HandleOutWithType(CompressionType type, std::shared_ptr<Buffer> buffer);
std::shared_ptr<Buffer> HandleInWithType(CompressionType type, std::shared_ptr<Buffer> buffer);
std::shared_ptr<Buffer> HandleInDynamic(std::shared_ptr<Buffer> buffer);

}

}
#endif  //ZNET_PARENT_COMPRESSION_H
