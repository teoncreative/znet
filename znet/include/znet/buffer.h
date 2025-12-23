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

#include "znet/inet_addr.h"
#include "znet/types.h"
#include "znet/util.h"
#include "znet/logger.h"
#include "znet/precompiled.h"

#include <bitset>

namespace znet {

enum class BufferError {
  None,
  WriteAfterSeal,
  CannotAllocate,
  ReadOutOfBounds,
  CorruptedFormat,
};

inline std::string GetBufferErrorString(BufferError error) {
  switch (error) {
    case BufferError::None:
      return "NoError";
    case BufferError::WriteAfterSeal:
      return "WriteAfterSeal";
    case BufferError::CannotAllocate:
      return "CannotAllocate";
    case BufferError::ReadOutOfBounds:
      return "ReadOutOfBounds";
    case BufferError::CorruptedFormat:
      return "CorruptedFormat";
    default:
      return "Unknown";
  }
}

class Buffer {
 public:
  explicit Buffer(Endianness endianness = Endianness::LittleEndian) {
    last_error_ = BufferError::None;
    endianness_ = endianness;
    read_cursor_ = 0;
    write_cursor_ = 0;
    allocated_size_ = 0;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 0;
#endif
    data_ = nullptr;
  }

  Buffer(const char* data, int data_size,
         Endianness endianness = Endianness::LittleEndian) {
    last_error_ = BufferError::None;
    endianness_ = endianness;
    read_cursor_ = 0;
    write_cursor_ = data_size;
    allocated_size_ = data_size;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 1;
#endif
    data_ = new (std::nothrow) char[allocated_size_];
    if (!data_) {
      last_error_ = BufferError::CannotAllocate;
      allocated_size_ = 0;
      return;
    }
    std::memcpy(data_, data, write_cursor_);
  }

  ~Buffer() { delete[] data_; }

#ifdef ZNET_BUFFER_DISABLE_COPY
  Buffer(const Buffer&) = delete;
#else
  Buffer(const Buffer& buffer) {
#ifdef ZNET_BUFFER_WARN_COPY
    ZNET_LOG_WARN("Buffer copy constructor called!");
#endif
    endianness_ = buffer.endianness_;
    allocated_size_ = buffer.allocated_size_;
    write_cursor_ = 0;
    read_cursor_ = 0;
    last_error_ = BufferError::None;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 0;
#endif
    // todo safe errors
    data_ = new (std::nothrow) char[allocated_size_];
    if (!data_) {
      last_error_ = BufferError::CannotAllocate;
      allocated_size_ = 0;
      return;
    }
    std::memcpy(data_, buffer.data_, allocated_size_);
  }
#endif

  template<typename T, typename std::enable_if<std::is_arithmetic<T>::value && (sizeof(T) <= 8), int>::type = 0>
  void Read(T* arr, size_t size) {
    if (size > std::numeric_limits<size_t>::max() / sizeof(T)) {
      last_error_ = BufferError::CorruptedFormat;
      return;
    }
    char* pt = reinterpret_cast<char*>(arr);
    size_t calculated_size = sizeof(T) * size;
    if (!CheckReadableBytes(size)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return;
    }
    std::memcpy(pt, data_ + read_cursor_, calculated_size);
    read_cursor_ += calculated_size;
  }

  char ReadChar() { return ReadInt<char>(); }

  unsigned char ReadUnsignedChar() { return ReadInt<unsigned char>(); }

  bool ReadBool() { return ReadInt<bool>(); }

  float ReadFloat() { return ReadNumber<float>(); }

  double ReadDouble() { return ReadNumber<double>(); }

  template<typename T, typename std::enable_if<std::is_integral<T>::value && (sizeof(T) <= 16), int>::type = 0>
  T ReadInt() {
    return ReadNumber<T>();
  }

  template<typename T, typename std::enable_if<std::is_arithmetic<T>::value && (sizeof(T) <= 16), int>::type = 0>
  T ReadNumber() {
    size_t size = sizeof(T);
    std::unique_ptr<char[]> data(new (std::nothrow) char[size]);
    if (!data) {
      last_error_ = BufferError::CannotAllocate;
      return 0;
    }
    if (!CheckReadableBytes(size)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return 0;
    }
    if (GetSystemEndianness() == endianness_) {
      for (size_t i = 0; i < size; i++) {
        data[i] = data_[read_cursor_ + i];
      }
    } else {
      for (size_t i = size, j = 0; i > 0; i--, j++) {
        data[i - 1] = data_[read_cursor_ + j];
      }
    }
    read_cursor_ += size;
    T l = 0;
    std::memcpy(&l, data.get(), size);
    return l;
  }

  template<typename T, typename = void_t<decltype(T::Read())>>
  T ReadCustom() {
    return T::Read();
  }

  std::unique_ptr<InetAddress> ReadInetAddress() {
    auto ver = ReadInt<uint8_t>();
    if (ver == 4) {
      uint32_t raw_ip{};
      uint16_t raw_port{};
      Read(&raw_ip, 1);
      Read(&raw_port, 1);
      in_addr ip{};
      ip.s_addr = raw_ip;
      auto port = ntohs(raw_port);
      return std::make_unique<InetAddressIPv4>(ip, port);
    }
    if (ver == 6) {
      in6_addr ip6{};
      Read(ip6.s6_addr, 16);
      uint16_t raw_port{};
      Read(&raw_port, 1);
      auto port = ntohs(raw_port);
      return std::make_unique<InetAddressIPv6>(ip6, port);
    }
    ZNET_LOG_WARN("Invalid internet protocol version {}!", ver);
    // unknown version
    last_error_ = BufferError::CorruptedFormat;
    return nullptr;
  }

  template<size_t N>
  std::bitset<N> ReadBitset() {
    constexpr size_t BYTES = (N + 7) / 8;
    uint8_t data[BYTES];
    Read(data, BYTES);

    std::bitset<N> bs;
    for (size_t i = 0; i < N; ++i) {
      bool bit = (data[i/8] >> (i % 8)) & 1;
      bs.set(i, bit);
    }
    return bs;
  }

  PortNumber ReadPort() {
    return ReadInt<PortNumber>();
  }

  template<typename T, typename std::enable_if<std::is_arithmetic<T>::value && (sizeof(T) <= 8), int>::type = 0>
  T ReadVarInt() {
    uint8_t size = sizeof(T);
    char* data = new (std::nothrow) char[size];
    if (!data) {
      last_error_ = BufferError::CannotAllocate;
      return 0;
    }
    std::memset(data, 0, size);
    if (!CheckReadableBytes(1)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return 0;
    }
    uint8_t actual_size = ReadChar();
    if (!CheckReadableBytes(actual_size)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return 0;
    }
    if (GetSystemEndianness() == endianness_) {
      for (size_t i = 0; i < actual_size; i++) {
        data[i] = data_[read_cursor_ + i];
      }
    } else {
      for (size_t i = actual_size, j = 0; i > 0; i--, j++) {
        data[i - 1] = data_[read_cursor_ + j];
      }
    }
    read_cursor_ += actual_size;
    T l = 0;
    std::memcpy(&l, data, size);
    return l;
  }

  std::string ReadString() {
    size_t size = ReadVarInt<size_t>();
    if (!CheckReadableBytes(size)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return "";
    }
    char* data = new (std::nothrow) char[size];
    if (!data) {
      last_error_ = BufferError::CannotAllocate;
      return "";
    }
    for (size_t i = 0; i < size; i++) {
      data[i] = ReadInt<char>();
    }
    return {data, size};
  }

  template <typename Map, typename KeyFunc, typename ValueFunc>
  Map ReadMap(KeyFunc key_func, ValueFunc value_func) {
    size_t size = ReadVarInt<size_t>();
    Map map;
    for (int i = 0; i < size; i++) {
      auto key = (this->*key_func)();
      auto value = (this->*value_func)();
      map[key] = value;
    }
    return map;
  }

  template <typename T, typename ValueFunc>
  std::vector<T> ReadVector(ValueFunc value_func) {
    size_t size = ReadVarInt<size_t>();
    std::vector<T> v;
    v.reserve(size);
    for (int i = 0; i < size; i++) {
      v.push_back((this->*value_func)());
    }
    return v;
  }

  template <typename T, typename ValueFunc>
  std::unique_ptr<T[]> ReadArray(ValueFunc value_func) {
    size_t size = ReadVarInt<size_t>();
    size_t size_bytes = size * sizeof(T);
    if (!CheckReadableBytes(size_bytes)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return nullptr;
    }
    T* ptr = new T[size];
    if (!ptr) {
      last_error_ = BufferError::CannotAllocate;
      return nullptr;
    }
    std::unique_ptr<T[]> array(ptr);
    for (int i = 0; i < size; i++) {
      array[i] = (this->*value_func)();
    }
    return array;
  }

  template <typename T, size_t size, typename ValueFunc>
  std::array<T, size> ReadArray(ValueFunc value_func) {
    size_t size_r = ReadVarInt<size_t>();
    if (size_r != size) {
      ZNET_LOG_ERROR("Array size mismatch. Expected: {}, Actual: {}", size,
                     size_r);
      last_error_ = BufferError::CorruptedFormat;
      return {};
    }
    size_t size_bytes = size * sizeof(T);
    if (!CheckReadableBytes(size_bytes)) {
      last_error_ = BufferError::ReadOutOfBounds;
      return {};
    }
    std::array<T, size> array;
    for (size_t i = 0; i < size; i++) {
      array[i] = (this->*value_func)();
    }
    return array;
  }

  void WriteString(const std::string& str) {
    if (!CheckSeal()) {
      return;
    }
    size_t size = str.size();
    const char* data = str.data();
    ReserveIncremental(size + sizeof(size));
    WriteVarInt(size);
    for (size_t i = 0; i < size; i++) {
      WriteInt(data[i]);
    }
  }

  void WriteChar(char c) { WriteInt(c); }

  void WriteUnsignedChar(unsigned char c) { WriteInt(c); }

  void WriteBool(bool b) { WriteInt(b); }

  void WriteFloat(float f) { WriteNumber(f); }

  void WriteDouble(double f) { WriteNumber(f); }

  template<typename T, typename std::enable_if<std::is_integral<T>::value && (sizeof(T) <= 16), int>::type = 0>
  void WriteInt(T c) {
    return WriteNumber(c);
  }

  template<typename T, typename std::enable_if<std::is_arithmetic<T>::value && (sizeof(T) <= 16), int>::type = 0>
  void WriteNumber(T c) {
    if (!CheckSeal()) {
      return;
    }
    char* pt = reinterpret_cast<char*>(&c);
    size_t size = sizeof(c);
    ReserveIncremental(size);
    if (GetSystemEndianness() == endianness_) {
      for (size_t i = 0; i < size; i++) {
        data_[write_cursor_ + i] = pt[i];
      }
    } else {
      for (size_t i = size, j = 0; i > 0; i--, j++) {
        data_[write_cursor_ + j] = pt[i - 1];
      }
    }
    write_cursor_ += size;
  }

  template<typename T, typename = void_t<decltype(T::Write())>>
  T WriteCustom() {
    if (!CheckSeal()) {
      return;
    }
    return T::Write(*this);
  }

  void WriteInetAddress(InetAddress& address) {
    if (!CheckSeal()) {
      return;
    }
    if (address.ipv() == InetProtocolVersion::IPv4) {
      WriteInt<uint8_t>(4);
      // raw IPv4 (network‐order) + port (network‐order)
      auto* addr = reinterpret_cast<sockaddr_in*>(address.handle_ptr());
      Write(reinterpret_cast<const uint32_t*>(&addr->sin_addr.s_addr), 1);
      Write(reinterpret_cast<const uint16_t*>(&addr->sin_port), 1);
    } else if (address.ipv() == InetProtocolVersion::IPv6) {
      WriteInt<uint8_t>(6);
      auto* addr = reinterpret_cast<sockaddr_in6*>(address.handle_ptr());
      // raw IPv6 (16 bytes) + port
      Write(addr->sin6_addr.s6_addr, 16);
      Write(reinterpret_cast<const uint16_t*>(&addr->sin6_port), 1);
    } else {
      WriteInt<uint8_t>(0);
    }
  }

  void WritePort(PortNumber port) {
    // Pretty funny but this should be enough
    WriteInt(port);
  }

  // write a std::bitset<N> (little‑endian bit order)
  template<size_t N>
  void WriteBitset(const std::bitset<N>& bs) {
    if (!CheckSeal()) {
      return;
    }
    constexpr size_t BYTES = (N + 7) / 8;
    uint8_t data[BYTES] = {};
    for (size_t i = 0; i < N; ++i) {
      if (bs[i]) {
        data[i/8] |= uint8_t(1u << (i % 8));
      }
    }
    Write(data, BYTES);
  }

  template<typename T, typename std::enable_if<std::is_arithmetic<T>::value && (sizeof(T) <= 8), int>::type = 0>
  void Write(T* arr, size_t size) {
    if (!CheckSeal()) {
      return;
    }
    auto* pt = reinterpret_cast<const char*>(arr);
    size_t calculated_size = sizeof(T) * size;
    ReserveIncremental(calculated_size);
    std::memcpy(data_ + write_cursor_, pt, calculated_size);
    write_cursor_ += calculated_size;
  }

  template<typename T, typename std::enable_if<std::is_arithmetic<T>::value && (sizeof(T) <= 8), int>::type = 0>
  void WriteVarInt(T c) {
    if (!CheckSeal()) {
      return;
    }
    char* pt = reinterpret_cast<char*>(&c);
    uint8_t size = sizeof(c);  // assume 1 byte for the size
    uint8_t actual_size = 0;
    for (size_t i = 0; i < size; i++) {
      if (pt[i] != 0) {
        actual_size = i + 1;
      }
    }
    ReserveIncremental(actual_size + 1);
    WriteInt((uint8_t)actual_size);
    if (GetSystemEndianness() == endianness_) {
      for (size_t i = 0; i < actual_size; i++) {
        data_[write_cursor_ + i] = pt[i];
      }
    } else {
      for (size_t i = actual_size, j = 0; i > 0; i--, j++) {
        data_[write_cursor_ + j] = pt[i - 1];
      }
    }
    write_cursor_ += actual_size;
  }

  template <typename KeyFunc, typename ValueFunc, typename Map>
  void WriteMap(Map& map, KeyFunc key_func, ValueFunc value_func) {
    if (!CheckSeal()) {
      return;
    }
    WriteInt(map.size());
    for (auto& kv : map) {
      (this->*key_func)(kv.first);
      (this->*value_func)(kv.second);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteVector(std::vector<T>& v, ValueFunc value_func) {
    if (!CheckSeal()) {
      return;
    }
    size_t size = v.size();
    WriteInt(size);
    for (auto& value : v) {
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T, size_t size>
  void WriteArray(T (&v)[size], ValueFunc value_func) {
    if (!CheckSeal()) {
      return;
    }
    WriteInt(size);
    for (int i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteArray(T* v, size_t size, ValueFunc value_func) {
    if (!CheckSeal()) {
      return;
    }
    if (!v) {
      WriteInt(0);
      return;
    }
    WriteInt(size);
    for (int i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteArray(std::shared_ptr<T[]>& v, size_t size, ValueFunc value_func) {
    if (!CheckSeal()) {
      return;
    }
    WriteInt(size);
    for (int i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  std::string Dump(int width = 2, int wrap = 8) {
    std::string str;
    for (int i = 0; i < write_cursor_; i++) {
      if (i != 0) {
        if (i % wrap == 0) {
          str += "\n";
        } else {
          str += " ";
        }
      }
      str += ToHex((uint8_t)data_[i], width);
    }
    return str;
  }

  void Trim() {
    if (write_cursor_ == allocated_size_) {
      return;
    }
    if (!CheckSeal()) {
      return;
    }
    char* new_data = new (std::nothrow) char[write_cursor_];
    if (!new_data) {
      last_error_ = BufferError::CannotAllocate;
      return;
    }
    std::memcpy(new_data, data_, write_cursor_);
    delete[] data_;
    data_ = new_data;
    allocated_size_ = write_cursor_;
  }

  void Reset(bool deallocate = false) {
    if (!CheckSeal()) {
      return;
    }
    write_cursor_ = 0;
    read_cursor_ = 0;
    last_error_ = BufferError::None;
    if (deallocate) {
      allocated_size_ = 0;
      delete[] data_;
      data_ = nullptr;
    }
  }

  void set_endianness(Endianness endianness) { endianness_ = endianness; }

  const char* data() { return data_; }

  const char* read_cursor_data() { return data_ + read_cursor_; }

  char* data_mutable() { return data_; }

  char* read_cursor_data_mutable() { return data_ + read_cursor_; }

  ZNET_NODISCARD size_t write_cursor() const { return write_cursor_; }

  void set_write_cursor(size_t cursor) { write_cursor_ = cursor; }

  void set_read_cursor(size_t cursor) { read_cursor_ = cursor; }

  ZNET_NODISCARD size_t read_cursor() const { return read_cursor_; }

  ZNET_NODISCARD size_t size() const { return write_cursor_; }

  ZNET_NODISCARD size_t capacity() const { return allocated_size_; }

  ZNET_NODISCARD ssize_t readable_bytes() const {
    return std::min(write_cursor_, read_limit_) - read_cursor_;
  }

  ZNET_NODISCARD size_t writable_bytes() const {
    return allocated_size_ - write_cursor_;
  }

#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
  ZNET_NODISCARD size_t mem_allocations() const { return mem_allocations_; }
#endif

  void SkipRead(size_t size) { read_cursor_ += size; }

  void SetReadLimit(size_t limit) {
    if (limit == 0) {
      read_limit_ = std::numeric_limits<size_t>::max();
    } else {
      read_limit_ = limit;
    }
  }

  void SkipWrite(size_t size) {
    if (sealed_.load(std::memory_order_acquire)) {
      last_error_ = BufferError::WriteAfterSeal;
      return;
    }
    ReserveIncremental(size);
    write_cursor_ += size;
  }

  /**
   * @return Returns the previous error and clears it
   */
  ZNET_NODISCARD BufferError GetAndClearLastError() {
    BufferError error = last_error_;
    last_error_ = BufferError::None;
    return error;
  }

  void ReserveIncremental(size_t additional_bytes) {
    Reserve(write_cursor_ + additional_bytes);
  }

  void ReserveExact(size_t size) { Reserve(size, true); }

  void Reserve(size_t size, bool exact = false) {
    if (!CheckSeal()) {
      return;
    }
    if (!data_) {
      size_t target_size;
      if (exact) {
        target_size = size;
      } else {
        target_size = size * 2;
      }
      data_ = new (std::nothrow) char[target_size];
      if (!data_) {
        last_error_ = BufferError::CannotAllocate;
        return;
      }
      allocated_size_ = target_size;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
      mem_allocations_++;
#endif
      return;
    }
    if (allocated_size_ >= size) {
      return;
    }
    size_t target_size_ = size * 2;
    char* tmp_data = new (std::nothrow) char[target_size_];
    if (!tmp_data) {
      last_error_ = BufferError::CannotAllocate;
      return;
    }
    allocated_size_ = target_size_;
    std::memcpy(tmp_data, data_, write_cursor_);
    delete[] data_;
    data_ = tmp_data;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_++;
#endif
  }

  void Seal() {
    sealed_.store(true, std::memory_order_release);
  }

 private:
  ZNET_NODISCARD bool CheckReadableBytes(size_t required) const {
#if defined(DEBUG) && !defined(DISABLE_ASSERT_READABLE_BYTES)
    assert(std::min(write_cursor_, read_limit_) >= read_cursor_ + required);
#endif
    return std::min(write_cursor_, read_limit_) >= read_cursor_ + required;
  }
  ZNET_NODISCARD inline bool CheckSeal() {
    if (sealed_.load(std::memory_order_acquire)) {
      ZNET_LOG_DEBUG("Tried to write to a sealed buffer.");
      last_error_ = BufferError::WriteAfterSeal;
      return false;
    }
    return true;
  }

  Endianness endianness_;
  size_t allocated_size_;
  size_t write_cursor_;
  size_t read_cursor_;
  size_t read_limit_ = std::numeric_limits<size_t>::max();
  char* data_;
  BufferError last_error_;
  std::atomic_bool sealed_{false};
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
  size_t mem_allocations_;
#endif
};
}  // namespace znet