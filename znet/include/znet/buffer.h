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

#include <bit>

#include "base/types.h"
#include "logger.h"
#include "znet/base/util.h"

namespace znet {

// For numbers, sizeof(T) is guarantied to be less than 8 bytes, but we
// still do the check.
template <typename T>
concept Number = std::is_arithmetic_v<T> && sizeof(T) <= 8;

class Buffer {
 public:
  explicit Buffer(Endianness endianness = Endianness::LittleEndian) {
    failed_to_read_ = false;
    endianness_ = endianness;
    read_cursor_ = 0;
    write_cursor_ = 0;
    allocated_size_ = 0;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 0;
#endif
    data_ = nullptr;
  }

  Buffer(char* data, int data_size,
         Endianness endianness = Endianness::LittleEndian) {
    failed_to_read_ = false;
    endianness_ = endianness;
    read_cursor_ = 0;
    write_cursor_ = data_size;
    allocated_size_ = data_size;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 1;
#endif
    data_ = new char[allocated_size_];
    memcpy(data_, data, write_cursor_);
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
    failed_to_read_ = false;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 0;
#endif
    data_ = new char[allocated_size_];
    memcpy(data_, buffer.data_, allocated_size_);
  }
#endif

  char ReadChar() { return ReadInt<char>(); }

  bool ReadBool() { return ReadInt<bool>(); }

  template <Number T>
  T ReadInt() {
    size_t size = sizeof(T);
    char* data = new char[size];
    if (!CheckReadableBytes(size)) {
      failed_to_read_ = true;
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
    memcpy(&l, data, size);
    return l;
  }

  template <Number T>
  T ReadVarInt() {
    uint8_t size = sizeof(T);
    char* data = new char[size];
    if (!CheckReadableBytes(1)) {
      failed_to_read_ = true;
      return 0;
    }
    uint8_t actual_size = ReadChar();
    if (!CheckReadableBytes(actual_size)) {
      failed_to_read_ = true;
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
    read_cursor_ += size;
    T l = 0;
    memcpy(&l, data, size);
    return l;
  }

  std::string ReadString() {
    Number auto size = ReadInt<size_t>();
    if (!CheckReadableBytes(size))
      failed_to_read_ = true;
    char* data = new char[size];
    for (size_t i = 0; i < size; i++) {
      data[i] = ReadInt<char>();
    }
    return {data, size};
  }

  template <typename Map, typename KeyFunc, typename ValueFunc>
  Map ReadMap(KeyFunc key_func, ValueFunc value_func) {
    Number auto size = ReadInt<size_t>();
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
    Number auto size = ReadInt<size_t>();
    std::vector<T> v;
    v.reserve(size);
    for (int i = 0; i < size; i++) {
      v.push_back((this->*value_func)());
    }
    return v;
  }

  template <typename T, typename ValueFunc>
  Ref<T[]> ReadArray(ValueFunc value_func) {
    Number auto size = ReadInt<size_t>();
    Ref<T[]> array = CreateRef<T[]>(size);
    for (int i = 0; i < size; i++) {
      array[i] = (this->*value_func)();
    }
    return array;
  }

  template <typename T, size_t size, typename ValueFunc>
  std::array<T, size> ReadArray(ValueFunc value_func) {
    Number auto size_r = ReadInt<size_t>();
    if (size_r != size) {
      ZNET_LOG_ERROR("Array size mismatch. Expected: {}, Actual: {}", size,
                     size_r);
      failed_to_read_ = true;
      return {};
    }
    std::array<T, size> array;
    for (size_t i = 0; i < size; i++) {
      array[i] = (this->*value_func)();
    }
    return array;
  }

  void WriteString(const std::string& str) {
    size_t size = str.size();
    const char* data = str.data();
    ReserveIncremental(size + sizeof(size));
    WriteInt(size);
    for (size_t i = 0; i < size; i++) {
      WriteInt(data[i]);
    }
  }

  void WriteChar(char c) { WriteInt(c); }

  void WriteBool(bool b) { WriteInt(b); }

  template <Number T>
  void WriteInt(T c) {
    char* pt = reinterpret_cast<char*>(&c);
    uint8_t size = sizeof(c);
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

  template <Number T>
  void WriteVarInt(T c) {
    char* pt = reinterpret_cast<char*>(&c);
    uint8_t size = sizeof(c);  // assume 1 byte for the size
    uint8_t actual_size = 0;
    for (size_t i = 0; i < size; i++) {
      if (pt[i] != 0) {
        actual_size = i;
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
    size_t size = map.size();
    WriteInt(size);
    for (auto& [key, value] : map) {
      (this->*key_func)(key);
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteVector(std::vector<T>& v, ValueFunc value_func) {
    size_t size = v.size();
    WriteInt(size);
    for (auto& value : v) {
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T, size_t size>
  void WriteArray(T (&v)[size], ValueFunc value_func) {
    WriteInt(size);
    for (int i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteArray(T* v, size_t size, ValueFunc value_func) {
    WriteInt(size);
    for (int i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteArray(Ref<T[]>& v, size_t size, ValueFunc value_func) {
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
    char* new_data = new char[write_cursor_];
    memcpy(new_data, data_, write_cursor_);
    delete[] data_;
    data_ = new_data;
    allocated_size_ = write_cursor_;
  }

  void set_endianness(Endianness endianness) { endianness_ = endianness; }

  const char* data() { return data_; }

  ZNET_NODISCARD size_t write_cursor() const { return write_cursor_; }

  void set_write_cursor(size_t cursor) { write_cursor_ = cursor; }

  ZNET_NODISCARD size_t read_cursor() const { return read_cursor_; }

  ZNET_NODISCARD size_t size() const { return write_cursor_; }

  ZNET_NODISCARD size_t capacity() const { return allocated_size_; }

  ZNET_NODISCARD ssize_t readable_bytes() const {
    return write_cursor_ - read_cursor_;
  }

  ZNET_NODISCARD size_t writable_bytes() const {
    return allocated_size_ - write_cursor_;
  }

#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
  ZNET_NODISCARD size_t mem_allocations() const { return mem_allocations_; }
#endif

  void SkipRead(size_t size) { read_cursor_ += size; }

  void SkipWrite(size_t size) {
    ReserveIncremental(size);
    write_cursor_ += size;
  }

  /**
   * @return true if previous read call was failed and clears the value.
   */
  ZNET_NODISCARD bool IsFailedToRead() {
    bool failed_to_read = failed_to_read_;
    failed_to_read_ = false;
    return failed_to_read;
  }

  void ReserveIncremental(size_t additional_bytes) {
    Reserve(write_cursor_ + additional_bytes);
  }

  void ReserveExact(size_t size) { Reserve(size, true); }

  void Reserve(size_t size, bool exact = false) {
    if (!data_) {
      if (exact) {
        allocated_size_ = size;
      } else {
        allocated_size_ = size * 2;
      }
      data_ = new char[allocated_size_];
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
      mem_allocations_++;
#endif
      return;
    }
    if (allocated_size_ >= size) {
      return;
    }
    allocated_size_ = size * 2;
    char* tmp_data = new char[allocated_size_];
    memcpy(tmp_data, data_, write_cursor_);
    delete[] data_;
    data_ = tmp_data;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_++;
#endif
  }

 private:
  ZNET_NODISCARD bool CheckReadableBytes(size_t required) const {
    size_t bytes_left = write_cursor_ - read_cursor_;
#if defined(DEBUG) && !defined(DISABLE_ASSERT_READABLE_BYTES)
    assert(bytes_left >= required);
#endif
    return bytes_left >= required;
  }

  Endianness endianness_;
  size_t allocated_size_;
  size_t write_cursor_;
  size_t read_cursor_;
  char* data_;
  bool failed_to_read_;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
  size_t mem_allocations_;
#endif
};
}  // namespace znet