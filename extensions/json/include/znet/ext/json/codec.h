//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "znet/buffer.h"
#include "znet/compat.h"

namespace znet {
namespace ext {

/**
 * @brief Bounds applied to json arriving off the wire.
 *
 * Both matter, and the depth one is not optional. See ReadJson.
 */
struct JsonLimits {
  /** @brief Largest encoded payload accepted, before decoding. */
  size_t max_bytes = static_cast<size_t>(1) << 20;  // 1 MiB

  /** @brief Deepest nesting accepted. Real messages are nowhere near this. */
  int max_depth = 64;
};

namespace detail {

/**
 * @brief A SAX consumer that only counts nesting and refuses to go deeper.
 *
 * nlohmann's MessagePack reader is recursive, and it is reached directly from
 * network bytes. A payload of a hundred thousand repeated 0x91 bytes, which is
 * a hundred kilobytes, nests a hundred thousand arrays deep and overflows the
 * stack: a remote crash from a small packet, in a library that is otherwise
 * careful (its *text* parser is iterative and shrugs off millions of levels).
 *
 * Returning false from start_array/start_object makes the reader unwind
 * immediately rather than descend, so scanning with this first is bounded by
 * max_depth no matter what the payload claims. It builds nothing, so the scan
 * is cheap and cannot itself be made to allocate.
 */
class JsonDepthGuard : public nlohmann::json_sax<nlohmann::json> {
 public:
  explicit JsonDepthGuard(int limit) : limit_(limit) {}

  ZNET_NODISCARD bool exceeded() const { return exceeded_; }

  bool start_object(std::size_t) override { return Enter(); }
  bool start_array(std::size_t) override { return Enter(); }
  bool end_object() override {
    --depth_;
    return true;
  }
  bool end_array() override {
    --depth_;
    return true;
  }

  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(number_integer_t) override { return true; }
  bool number_unsigned(number_unsigned_t) override { return true; }
  bool number_float(number_float_t, const string_t&) override { return true; }
  bool string(string_t&) override { return true; }
  bool binary(binary_t&) override { return true; }
  bool key(string_t&) override { return true; }
  bool parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) override {
    return false;
  }

 private:
  bool Enter() {
    if (++depth_ > limit_) {
      exceeded_ = true;
      return false;
    }
    return true;
  }

  int depth_ = 0;
  int limit_;
  bool exceeded_ = false;
};

/** @brief True when @p bytes nests no deeper than @p max_depth. */
inline bool DepthWithinLimit(const std::vector<uint8_t>& bytes, int max_depth,
                             nlohmann::json::input_format_t format) {
  JsonDepthGuard guard(max_depth);
  // the return value also goes false on a malformed payload, which the real
  // parse below reports for itself; only the depth verdict is wanted here
  static_cast<void>(
      nlohmann::json::sax_parse(bytes, &guard, format, false));
  return !guard.exceeded();
}

/** @brief Reads a length-prefixed blob, refusing implausible lengths. */
inline bool ReadBlob(Buffer& buffer, std::vector<uint8_t>& out,
                     size_t max_bytes) {
  const size_t size = buffer.ReadVarInt<size_t>();
  if (size > max_bytes || size > buffer.readable_bytes()) {
    return false;
  }
  out.resize(size);
  if (size != 0) {
    buffer.Read(out.data(), size);
  }
  return true;
}

/** @brief Writes a length-prefixed blob. */
inline void WriteBlob(Buffer& buffer, const std::vector<uint8_t>& bytes) {
  buffer.WriteVarInt(bytes.size());
  if (!bytes.empty()) {
    buffer.Write(bytes.data(), bytes.size());
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// MessagePack, the default
// ---------------------------------------------------------------------------

/**
 * @brief Writes @p value as length-prefixed MessagePack.
 *
 * Roughly half the size of the text form and quicker to parse. A discarded
 * json writes as null rather than throwing.
 */
inline void WriteJson(Buffer& buffer, const nlohmann::json& value) {
  const std::vector<uint8_t> bytes = nlohmann::json::to_msgpack(
      value.is_discarded() ? nlohmann::json() : value);
  detail::WriteBlob(buffer, bytes);
}

/**
 * @brief Reads json written by WriteJson.
 *
 * @return false, leaving @p out untouched, when the payload is oversized,
 *         truncated, nested past @p limits.max_depth, or simply not valid
 *         MessagePack.
 *
 * Nothing here throws, whatever arrives: a malformed packet is an ordinary
 * wire condition, and an exception escaping into znet's decode path would take
 * down the session rather than the packet.
 */
inline bool ReadJson(Buffer& buffer, nlohmann::json& out,
                     const JsonLimits& limits = JsonLimits()) {
  std::vector<uint8_t> bytes;
  if (!detail::ReadBlob(buffer, bytes, limits.max_bytes)) {
    return false;
  }
  if (!detail::DepthWithinLimit(bytes, limits.max_depth,
                                nlohmann::json::input_format_t::msgpack)) {
    return false;
  }
  nlohmann::json parsed = nlohmann::json::from_msgpack(
      bytes, /*strict=*/true, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return false;
  }
  out = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// Text, for when a packet capture has to be readable
// ---------------------------------------------------------------------------

/** @brief Writes @p value as a length-prefixed UTF-8 json document. */
inline void WriteJsonText(Buffer& buffer, const nlohmann::json& value) {
  const std::string text =
      value.is_discarded() ? std::string("null") : value.dump();
  const std::vector<uint8_t> bytes(text.begin(), text.end());
  detail::WriteBlob(buffer, bytes);
}

/**
 * @brief Reads json written by WriteJsonText, under the same limits.
 *
 * The text parser is iterative, so depth costs memory rather than stack, but
 * the same bound is applied so both forms accept exactly the same documents.
 */
inline bool ReadJsonText(Buffer& buffer, nlohmann::json& out,
                         const JsonLimits& limits = JsonLimits()) {
  std::vector<uint8_t> bytes;
  if (!detail::ReadBlob(buffer, bytes, limits.max_bytes)) {
    return false;
  }
  if (!detail::DepthWithinLimit(bytes, limits.max_depth,
                                nlohmann::json::input_format_t::json)) {
    return false;
  }
  nlohmann::json parsed =
      nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return false;
  }
  out = std::move(parsed);
  return true;
}

}  // namespace ext
}  // namespace znet
