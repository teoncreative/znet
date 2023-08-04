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

namespace znet {

class Buffer {
 public:
  explicit Buffer(Endianness endianness = Endianness::LittleEndian) {
    failed_to_read_ = false;
    endianness_ = endianness;
    resize_chain_count_ = 0;
    read_cursor_ = 0;
    write_cursor_ = 0;
    allocated_size_ = 0;
    data_ = nullptr;
    AssureSize(0);
  }

  Buffer(char* data, int data_size,
         Endianness endianness = Endianness::LittleEndian) {
    failed_to_read_ = false;
    endianness_ = endianness;
    resize_chain_count_ = 0;
    read_cursor_ = 0;
    write_cursor_ = data_size;
    allocated_size_ = data_size;
    data_ = new char[allocated_size_];
    memcpy(data_, data, write_cursor_);
  }

  ~Buffer() { delete[] data_; }

  // todo add a way to copy buffers easily
  /**
   * Buffer copy constructors are deleted intentionally to prevent
   * accidental copying.
   */
  Buffer(const Buffer&) = delete;

  char ReadChar() { return ReadInt<char>(); }

  bool ReadBool() { return ReadInt<bool>(); }

  template <typename T>
  T ReadInt() {
    ssize_t size = sizeof(T);
    char data[size];
    if (!CheckReadableBytes(size))
      failed_to_read_ = true;
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

  std::string ReadString() {
    auto size = ReadInt<size_t>();
    if (!CheckReadableBytes(size))
      failed_to_read_ = true;
    char data[size];
    for (size_t i = 0; i < size; i++) {
      data[i] = ReadInt<char>();
    }
    return {data, size};
  }

  void WriteString(const std::string& str) {
    size_t size = str.size();
    const char* data = str.data();
    AssureSizeIncremental(size + sizeof(size));
    WriteInt(size);
    if (GetSystemEndianness() == endianness_) {
      for (size_t i = 0; i < size; i++) {
        WriteInt(data[i]);
      }
    } else {
      for (size_t i = size; i > 0; i--) {
        WriteInt(data[i - 1]);
      }
    }
  }

  void WriteChar(char c) { WriteInt(c); }

  void WriteBool(bool b) { WriteInt(b); }

  template <typename T>
  void WriteInt(T c) {
    char* pt = reinterpret_cast<char*>(&c);
    size_t size = sizeof(c);
    AssureSizeIncremental(size);
    if (GetSystemEndianness() == endianness_) {
      for (size_t i = 0; i < size; i++) {
        data_[write_cursor_ + i] = pt[i];
      }
    } else {
      for (size_t i = size; i > 0; i--) {
        data_[write_cursor_ + i - 1] = pt[i - 1];
      }
    }
    write_cursor_ += size;
  }

  std::string ToStr() { return {data_, write_cursor_}; }

  const char* data() { return data_; }

  ZNET_NODISCARD size_t write_cursor() const { return write_cursor_; }
  void SetWriteCursor(size_t cursor) { write_cursor_ = cursor; }
  ZNET_NODISCARD size_t read_cursor() const { return read_cursor_; }

  ZNET_NODISCARD size_t Size() const { return write_cursor_; }

  ZNET_NODISCARD ssize_t ReadableBytes() const {
    return write_cursor_ - read_cursor_;
  }

  void SkipRead(size_t size) { read_cursor_ += size; }
  void SkipWrite(size_t size) { write_cursor_ += size; }

  /**
   * @return true if previous read call was failed and clears the value.
   */
  ZNET_NODISCARD bool IsFailedToRead() {
    bool failed_to_read = failed_to_read_;
    failed_to_read_ = false;
    return failed_to_read;
  }

 private:
  ZNET_NODISCARD bool CheckReadableBytes(size_t required) const {
    size_t bytes_left = write_cursor_ - read_cursor_;
#if defined(DEBUG) && !defined(DISABLE_ASSERT_READABLE_BYTES)
    assert(bytes_left >= required);
#endif
    return bytes_left >= required;
  }

  void AssureSizeIncremental(size_t additional_bytes) {
    AssureSize(write_cursor_ + additional_bytes);
  }

  void AssureSize(size_t size) {
    if (!data_) {
      allocated_size_ = size;
      data_ = new char[allocated_size_];
      return;
    }
    if (allocated_size_ >= size) {
      resize_chain_count_ = 0;
      return;
    }
    resize_chain_count_++;
    size_t additional_size = 16;
    if (resize_chain_count_ > 2) {
      additional_size += resize_chain_count_ * 16;
    }
    allocated_size_ = size + additional_size;
    char* tmp_data = new char[allocated_size_];
    memcpy(tmp_data, data_, write_cursor_);
    delete[] data_;
    data_ = tmp_data;
  }

  Endianness endianness_;
  size_t allocated_size_;
  size_t write_cursor_;
  size_t read_cursor_;
  char* data_;
  int resize_chain_count_;
  bool failed_to_read_;
};
}  // namespace znet