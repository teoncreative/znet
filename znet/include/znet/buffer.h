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

namespace znet {

  // todo endian modes
  // current issue: endianness can change between computers
  // we are assuming both sides use the same memory endianness by copying things around.
  class Buffer {
  public:
    explicit Buffer(Endianness endianness = Endianness::LittleEndian) {
      endianness_ = endianness;
      resize_chain_count_ = 0;
      read_cursor_ = 0;
      write_cursor_ = 0;
      allocated_size_ = 0;
      data_ = nullptr;
      AssureSize(0);
    }

    Buffer(char* data, int data_size, Endianness endianness = Endianness::LittleEndian) {
      endianness_ = endianness;
      resize_chain_count_ = 0;
      read_cursor_ = 0;
      write_cursor_ = data_size;
      allocated_size_ = data_size;
      data_ = new char[allocated_size_];
      memcpy(data_, data, write_cursor_);
    }

    ~Buffer() {
      delete[] data_;
    }

    template<typename T>
    T ReadInt() {
      ssize_t size = sizeof(T);
      char data[size];
      CheckReadableBytes(size);
      for (ssize_t i = 0; i < size; i++) {
        data[i] = data_[read_cursor_ + i];
      }
      read_cursor_ += size;
      long l;
      memcpy(&l, data, size);
      return l;
    }

    std::string ReadString() {
      size_t size = ReadInt<size_t>();
      CheckReadableBytes(size);
      char data[size];
      for (int i = 0; i < size; i++) {
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

    void WriteChar(char c) {
      WriteInt(c);
    }

    template<typename T>
    void WriteInt(T c) {
      char* pt = reinterpret_cast<char*>(&c);
      ssize_t size = sizeof(c);
      AssureSizeIncremental(size);
      if (GetSystemEndianness() == endianness_) {
        for (ssize_t i = 0; i < size; i++) {
          data_[write_cursor_ + i] = pt[i];
        }
      } else {
        for (ssize_t i = size; i > 0; i--) {
          data_[write_cursor_ + i - 1] = pt[i - 1];
        }
      }
      write_cursor_ += size;
    }

    std::string ToStr() {
      return {data_, write_cursor_};
    }

    const char* data() { return data_; }
    int write_cursor() const { return write_cursor_; }
    int size() const { return write_cursor_; }

  private:
    void CheckReadableBytes(ssize_t required) {
      size_t bytes_left = write_cursor_ - read_cursor_;
      if (bytes_left < required) {
        throw std::runtime_error("not enough bytes to read!");
      }
    }

    void AssureSizeIncremental(ssize_t additional_bytes) {
      AssureSize(allocated_size_ + additional_bytes);
    }

    void AssureSize(ssize_t size) {
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
      ssize_t additional_size = 16;
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
    ssize_t allocated_size_;
    size_t write_cursor_;
    size_t read_cursor_;
    char* data_;
    int resize_chain_count_;

  };
}