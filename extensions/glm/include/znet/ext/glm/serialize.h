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
// exact (lossless) Buffer read/write for glm's vector, matrix and quaternion
// types.
//
// these are free functions rather than Buffer members so that the extension
// adds nothing to the core library and nothing to the include cost of a
// consumer that does not use glm.
//
// error reporting follows the Buffer convention: a read that runs off the end
// leaves the destination partially written and records the failure on the
// buffer, so the caller checks Buffer::GetAndClearLastError() once after
// deserializing a packet rather than after every field. The one exception is
// ReadVecArray, which returns a bool because it decides how much to allocate
// from a count that came off the wire.
//

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

#include "znet/buffer.h"

namespace znet {
namespace ext {

namespace detail {

/**
 * @brief How one component of a glm type crosses the wire.
 *
 * Everything arithmetic goes through Buffer::WriteNumber/ReadNumber, which
 * already applies the buffer's endianness.
 */
template <typename T>
struct Component {
  static void Write(Buffer& buffer, T value) { buffer.WriteNumber(value); }
  static T Read(Buffer& buffer) { return buffer.ReadNumber<T>(); }
};

/**
 * @brief bool is not memcpy-safe off the wire.
 *
 * ReadNumber copies the raw byte into the destination, and a bool holding
 * anything but 0 or 1 is undefined: both `b` and `!b` can test true. A peer
 * (or a corrupted frame) can trivially produce such a byte, so glm::bvec
 * components are normalised through an integer instead.
 */
template <>
struct Component<bool> {
  static void Write(Buffer& buffer, bool value) {
    buffer.WriteInt<uint8_t>(static_cast<uint8_t>(value ? 1 : 0));
  }
  static bool Read(Buffer& buffer) { return buffer.ReadInt<uint8_t>() != 0; }
};

/**
 * @brief Bytes one component occupies on the wire.
 *
 * Not sizeof(T) for bool: the specialisation above sends exactly one byte
 * whatever sizeof(bool) happens to be on the host.
 */
template <typename T>
struct ComponentWireSize : std::integral_constant<size_t, sizeof(T)> {};

template <>
struct ComponentWireSize<bool> : std::integral_constant<size_t, 1> {};

/** @brief Bytes one value of a glm vector type occupies on the wire. */
template <typename VecT>
size_t VecWireSize() {
  return ComponentWireSize<typename VecT::value_type>::value *
         static_cast<size_t>(VecT::length());
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

/**
 * @brief Writes @p value as its components in x, y, z, w order.
 *
 * Works for every glm vector: vec2/3/4, dvec, ivec, uvec, bvec and the
 * sized variants (i8vec3, u16vec2, ...). Cost on the wire is exactly
 * sizeof(component) * length, with no length prefix, because the arity is
 * part of the type on both ends.
 */
template <glm::length_t L, typename T, glm::qualifier Q>
void WriteVec(Buffer& buffer, const glm::vec<L, T, Q>& value) {
  for (glm::length_t i = 0; i < L; ++i) {
    detail::Component<T>::Write(buffer, value[i]);
  }
}

/** @brief Reads a vector written by WriteVec into @p out. */
template <glm::length_t L, typename T, glm::qualifier Q>
void ReadVec(Buffer& buffer, glm::vec<L, T, Q>& out) {
  for (glm::length_t i = 0; i < L; ++i) {
    out[i] = detail::Component<T>::Read(buffer);
  }
}

/**
 * @brief Value-returning form: `auto v = ReadVec<glm::vec3>(buffer);`
 *
 * The result is zero-initialised first, so a truncated read yields zeroes in
 * the components that were not on the wire rather than whatever was on the
 * stack.
 */
template <typename VecT>
VecT ReadVec(Buffer& buffer) {
  VecT out(typename VecT::value_type(0));
  ReadVec(buffer, out);
  return out;
}

// ---------------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------------

/**
 * @brief Writes @p value column by column, which is glm's own storage order.
 *
 * Column-major is chosen so that a mat4 on the wire is byte-identical to the
 * mat4 in memory on a little-endian host; it is not a transpose of it.
 */
template <glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
void WriteMat(Buffer& buffer, const glm::mat<C, R, T, Q>& value) {
  for (glm::length_t c = 0; c < C; ++c) {
    WriteVec(buffer, value[c]);
  }
}

/** @brief Reads a matrix written by WriteMat into @p out. */
template <glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
void ReadMat(Buffer& buffer, glm::mat<C, R, T, Q>& out) {
  for (glm::length_t c = 0; c < C; ++c) {
    ReadVec(buffer, out[c]);
  }
}

/** @brief Value-returning form: `auto m = ReadMat<glm::mat4>(buffer);` */
template <typename MatT>
MatT ReadMat(Buffer& buffer) {
  MatT out(typename MatT::value_type(0));
  ReadMat(buffer, out);
  return out;
}

// ---------------------------------------------------------------------------
// Quaternions
// ---------------------------------------------------------------------------

/**
 * @brief Writes @p value as x, y, z, w.
 *
 * The components are named explicitly rather than indexed: GLM_FORCE_QUAT_DATA_XYZW
 * changes what `q[0]` means, and two peers are allowed to have built glm with
 * different settings. Naming them pins the wire format to the maths, not to
 * the memory layout.
 */
template <typename T, glm::qualifier Q>
void WriteQuat(Buffer& buffer, const glm::qua<T, Q>& value) {
  detail::Component<T>::Write(buffer, value.x);
  detail::Component<T>::Write(buffer, value.y);
  detail::Component<T>::Write(buffer, value.z);
  detail::Component<T>::Write(buffer, value.w);
}

/** @brief Reads a quaternion written by WriteQuat into @p out. */
template <typename T, glm::qualifier Q>
void ReadQuat(Buffer& buffer, glm::qua<T, Q>& out) {
  out.x = detail::Component<T>::Read(buffer);
  out.y = detail::Component<T>::Read(buffer);
  out.z = detail::Component<T>::Read(buffer);
  out.w = detail::Component<T>::Read(buffer);
}

/** @brief Value-returning form: `auto q = ReadQuat<glm::quat>(buffer);` */
template <typename QuatT>
QuatT ReadQuat(Buffer& buffer) {
  QuatT out(typename QuatT::value_type(1), typename QuatT::value_type(0),
            typename QuatT::value_type(0), typename QuatT::value_type(0));
  ReadQuat(buffer, out);
  return out;
}

// ---------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------

/**
 * @brief Writes a varint count followed by each element.
 *
 * The count is a varint for the same reason WriteString's is: a mesh with
 * eight vertices should not spend eight bytes saying so.
 */
template <glm::length_t L, typename T, glm::qualifier Q>
void WriteVecArray(Buffer& buffer, const std::vector<glm::vec<L, T, Q>>& value) {
  buffer.WriteVarInt(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    WriteVec(buffer, value[i]);
  }
}

/**
 * @brief Reads a sequence written by WriteVecArray.
 *
 * @return false, leaving @p out empty, when the count does not match what is
 *         actually left in the buffer.
 *
 * The count is attacker-controlled, so it is checked against the bytes on hand
 * before anything is reserved: otherwise a nine-byte packet claiming four
 * billion vec3s would ask the allocator for 48 GB. This is why the function
 * reports through a return value instead of the buffer's error flag -- the
 * decision happens before any read that could set one.
 */
template <typename VecT>
bool ReadVecArray(Buffer& buffer, std::vector<VecT>& out) {
  out.clear();
  const size_t count = buffer.ReadVarInt<size_t>();
  if (count == 0) {
    return true;
  }
  const size_t element_size = detail::VecWireSize<VecT>();
  if (element_size == 0 || count > buffer.readable_bytes() / element_size) {
    return false;
  }
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(ReadVec<VecT>(buffer));
  }
  return true;
}

}  // namespace ext
}  // namespace znet
