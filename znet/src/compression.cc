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

#include "znet/compression.h"

namespace znet {

template <CompressionType Type>
struct CompressionCodec;

template <>
struct CompressionCodec<CompressionType::None> {
  static CompressionType type() { return CompressionType::None; }

  static std::shared_ptr<Buffer> HandleIn(std::shared_ptr<Buffer> buffer) {
    return buffer;
  }

  static std::shared_ptr<Buffer> HandleOut(std::shared_ptr<Buffer> buffer) {
    auto out = std::make_shared<Buffer>();
    out->ReserveExact(buffer->size() + 1);
    out->WriteInt(GetCompressionTypeRaw(type()));
    out->Write(buffer->data(), buffer->size());
    return out;
  }
};

CompressionTypeRaw GetCompressionTypeRaw(CompressionType type) {
  return static_cast<CompressionTypeRaw>(type);
}

std::string GetCompressionTypeName(CompressionType type) {
  switch (type) {
    case CompressionType::None:
      return "None";
    case CompressionType::Zstandard:
      return "Zstandard";
    default:
      return "Unknown";
  }
}

#ifdef ZNET_USE_ZSTD

#include "zstd.h"

std::shared_ptr<Buffer> DecompressZstd(std::shared_ptr<Buffer> buffer) {
  std::shared_ptr<Buffer> new_buffer = std::make_shared<Buffer>();

  size_t decompressed_bound = ZSTD_getFrameContentSize(
      buffer->read_cursor_data(), buffer->readable_bytes());
  if (decompressed_bound == ZSTD_CONTENTSIZE_ERROR ||
      decompressed_bound == ZSTD_CONTENTSIZE_UNKNOWN) {
    ZNET_LOG_ERROR("Failed to decompress buffer with zstd: {}",
                   ZSTD_getErrorName(decompressed_bound));
    return nullptr;
  }

  new_buffer->ReserveExact(decompressed_bound);
  size_t decompressed_size =
      ZSTD_decompress(new_buffer->data_mutable(), decompressed_bound,
                      buffer->read_cursor_data(), buffer->readable_bytes());

  if (ZSTD_isError(decompressed_size)) {
    ZNET_LOG_ERROR("Failed to decompress buffer with zstd: {}",
                   ZSTD_getErrorName(decompressed_bound));
    return nullptr;
  }

  new_buffer->set_write_cursor(decompressed_size);
  return new_buffer;
}

std::shared_ptr<Buffer> CompressZstd(std::shared_ptr<Buffer> buffer) {
  size_t max_size = ZSTD_compressBound(buffer->readable_bytes());

  auto new_buffer = std::make_shared<Buffer>();
  new_buffer->ReserveExact(max_size);

  size_t compressed_size =
      ZSTD_compress(new_buffer->data_mutable(), max_size,
                    buffer->read_cursor_data(), buffer->readable_bytes(),
                    2);  // compression level

  if (ZSTD_isError(compressed_size)) {
    ZNET_LOG_ERROR("Failed to compress buffer with zstd: {}",
                   ZSTD_getErrorName(compressed_size));
    return nullptr;
  }

  new_buffer->set_write_cursor(compressed_size);
  return new_buffer;
}

template <>
struct CompressionCodec<CompressionType::Zstandard> {
  static CompressionType type() { return CompressionType::Zstandard; }

  static std::shared_ptr<Buffer> HandleIn(std::shared_ptr<Buffer> buffer) {
    return DecompressZstd(buffer);
  }

  static std::shared_ptr<Buffer> HandleOut(std::shared_ptr<Buffer> buffer) {
    auto compressed = CompressZstd(buffer);
    if (!compressed) {
      return nullptr;
    }
    auto out = std::make_shared<Buffer>();
    out->ReserveExact(compressed->size() + sizeof(CompressionTypeRaw));
    out->WriteInt(GetCompressionTypeRaw(type()));
    out->Write(compressed->data(), compressed->size());
    return out;
  }
};
#endif

namespace compr {

std::shared_ptr<Buffer> HandleOutWithType(CompressionType type,
                                          std::shared_ptr<Buffer> buffer) {
  switch (type) {
    case CompressionType::None:
      return CompressionCodec<CompressionType::None>::HandleOut(buffer);
    case CompressionType::Zstandard: {
#ifdef ZNET_USE_ZSTD
      return CompressionCodec<CompressionType::Zstandard>::HandleOut(buffer);
#else
      static bool warnZstd = false;
      if (!warnZstd) {
        ZNET_LOG_WARN(
            "zstd compression is not available but tried to use zstd. Sent"
            "packets will be uncompressed. There will be no more warnings about "
            "further packets!");
        warnZstd = true;
      }
      return CompressionCodec<CompressionType::None>::HandleOut(buffer);
#endif
    }
    default:
      return nullptr;
  }
}

std::shared_ptr<Buffer> HandleInWithType(CompressionType type,
                                         std::shared_ptr<Buffer> buffer) {
  switch (type) {
    case CompressionType::None:
      return CompressionCodec<CompressionType::None>::HandleIn(buffer);
    case CompressionType::Zstandard: {
#ifdef ZNET_USE_ZSTD
      return CompressionCodec<CompressionType::Zstandard>::HandleIn(buffer);
#else
      static bool warnZstd = false;
      if (!warnZstd) {
        ZNET_LOG_WARN(
            "zstd compression is not available but a packet with zstd "
            "compression was received. Packet could not be decompressed. There "
            "will be no more warnings about further packets!");
        warnZstd = true;
      }
      return nullptr;
#endif
    }
    default:
      return nullptr;
  }
}

std::shared_ptr<Buffer> HandleInDynamic(std::shared_ptr<Buffer> buffer) {
  CompressionType type =
      static_cast<CompressionType>(buffer->ReadInt<CompressionTypeRaw>());
  return HandleInWithType(type, buffer);
}

}  // namespace compr

}  // namespace znet