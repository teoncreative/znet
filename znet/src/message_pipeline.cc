//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/message_pipeline.h"

#include "znet/encryption.h"
#include "znet/logger.h"

namespace znet {

std::shared_ptr<Buffer> MessagePipeline::Encode(
    const std::shared_ptr<Packet>& packet, uint8_t stream) {
  if (!codec_) {
    ZNET_LOG_WARN("Session {} has no codec, dropping packet!", id_);
    return nullptr;
  }
  auto buffer = codec_->Serialize(packet, kSendHeadroom);
  if (!buffer) {
    return nullptr;
  }
  // small messages skip compression: the coder tables cost more than they can
  // ever save back.
  CompressionType compression = out_compression_;
  if (buffer->readable_bytes() < compression_threshold_) {
    compression = CompressionType::None;
  }
  buffer = compr::HandleOutWithType(compression, std::move(buffer));
  if (!buffer) {
    ZNET_LOG_ERROR("Session {} compression failed, dropping packet!", id_);
    return nullptr;
  }
  buffer = encryption_.HandleOut(std::move(buffer), stream);
  if (!buffer) {
    ZNET_LOG_ERROR("Session {} encryption failed, dropping packet!", id_);
    return nullptr;
  }
  return buffer;
}

std::shared_ptr<Buffer> MessagePipeline::Decode(
    std::shared_ptr<Buffer> buffer) {
  buffer = encryption_.HandleIn(std::move(buffer));
  if (!buffer) {
    ZNET_LOG_ERROR("Session {} decryption returned null!", id_);
    return nullptr;
  }
  buffer = compr::HandleInDynamic(std::move(buffer));
  if (!buffer) {
    ZNET_LOG_ERROR("Session {} decompression returned null!", id_);
    return nullptr;
  }
  return buffer;
}

DecodeStats MessagePipeline::Dispatch(const std::shared_ptr<Buffer>& payload,
                                      PacketHandlerBase& handler) {
  return codec_->Deserialize(payload, handler, dump_on_decode_failure_);
}

}  // namespace znet
