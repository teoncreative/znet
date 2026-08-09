//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_BUFFER_H_
#define ZNET_BUFFER_H_

#include "znet/inet_addr.h"
#include "znet/types.h"
#include "znet/util.h"
#include "znet/logger.h"
#include "znet/precompiled.h"

#include <bitset>
#if ZNET_HAS_CXX20
#include <span>
#endif

namespace znet {

class Buffer;

// type constraints.
//
// each one exists twice: as a trait usable in any language mode, and (on
// C++20) as the concept of the same name that the public API used before
// C++14 support was added. the ZNET_TPL_* macros below pick whichever the
// compiler understands, so a C++20 build still gets concept diagnostics and a
// C++14 build gets the equivalent SFINAE constraint.
namespace detail {

template <typename T>
struct IsArithmetic8Byte
    : compat::BoolConstant<std::is_arithmetic<T>::value && sizeof(T) <= 8> {};

template <typename T>
struct IsArithmetic16Byte
    : compat::BoolConstant<std::is_arithmetic<T>::value && sizeof(T) <= 16> {};

template <typename T>
struct IsIntegral16Byte
    : compat::BoolConstant<std::is_integral<T>::value && sizeof(T) <= 16> {};

template <typename T, typename = void>
struct HasReadMethodT : std::false_type {};
template <typename T>
struct HasReadMethodT<T, compat::VoidT<decltype(T::Read())>>
    : std::is_same<decltype(T::Read()), T> {};

template <typename T, typename = void>
struct HasWriteMethodT : std::false_type {};
template <typename T>
struct HasWriteMethodT<T,
                       compat::VoidT<decltype(T::Write(std::declval<Buffer&>()))>>
    : std::is_same<decltype(T::Write(std::declval<Buffer&>())), T> {};

}  // namespace detail

// the concepts are aliases over the traits above rather than restatements of
// them, so each constraint is defined exactly once and the two spellings
// cannot drift apart.
#if ZNET_HAS_CXX20
template <typename T>
concept Arithmetic8Byte = detail::IsArithmetic8Byte<T>::value;

template <typename T>
concept Arithmetic16Byte = detail::IsArithmetic16Byte<T>::value;

template <typename T>
concept Integral16Byte = detail::IsIntegral16Byte<T>::value;

template <typename T>
concept HasReadMethod = detail::HasReadMethodT<T>::value;

template <typename T>
concept HasWriteMethod = detail::HasWriteMethodT<T>::value;
#endif

#define ZNET_TPL_ARITH8(T) \
  ZNET_TPL_CONSTRAINED(::znet::Arithmetic8Byte, ::znet::detail::IsArithmetic8Byte, T)
#define ZNET_TPL_ARITH16(T) \
  ZNET_TPL_CONSTRAINED(::znet::Arithmetic16Byte, ::znet::detail::IsArithmetic16Byte, T)
#define ZNET_TPL_INT16(T) \
  ZNET_TPL_CONSTRAINED(::znet::Integral16Byte, ::znet::detail::IsIntegral16Byte, T)
#define ZNET_TPL_HAS_READ(T) \
  ZNET_TPL_CONSTRAINED(::znet::HasReadMethod, ::znet::detail::HasReadMethodT, T)
#define ZNET_TPL_HAS_WRITE(T) \
  ZNET_TPL_CONSTRAINED(::znet::HasWriteMethod, ::znet::detail::HasWriteMethodT, T)

// ---------------------------------------------------------------------------
// Read limits
// ---------------------------------------------------------------------------
//
// every length on the wire is chosen by whoever sent it, and a reader that
// believes one has handed a stranger control of an allocation and a loop
// bound. these cap what a single length may claim.
//
// the cap is a ceiling, not the only guard: each read also refuses a count the
// remaining bytes could not possibly back, which needs no configuration and
// holds whatever these are set to. raising them widens what a peer may ask
// for, never what it may ask for without paying the bytes.
//
// set either to 0 to disable that ceiling, leaving only the bytes-on-hand
// check. sensible on a trusted link where a legitimate message really is
// larger than the default.

#ifndef ZNET_MAX_READ_ELEMENTS
#define ZNET_MAX_READ_ELEMENTS 65536
#endif

#ifndef ZNET_MAX_READ_STRING_LENGTH
#define ZNET_MAX_READ_STRING_LENGTH 65536
#endif

enum class BufferError {
  None,
  CannotAllocate,
  ReadOutOfBounds,
  CorruptedFormat,
  /** @brief A length on the wire exceeded the configured ZNET_MAX_READ_* cap. */
  ReadLimitExceeded,
};

inline std::string GetBufferErrorString(BufferError error) {
  switch (error) {
    case BufferError::None:
      return "NoError";
    case BufferError::CannotAllocate:
      return "CannotAllocate";
    case BufferError::ReadOutOfBounds:
      return "ReadOutOfBounds";
    case BufferError::CorruptedFormat:
      return "CorruptedFormat";
    case BufferError::ReadLimitExceeded:
      return "ReadLimitExceeded";
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

  Buffer(const char* data, size_t data_size,
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
    if (ZNET_UNLIKELY(!data_)) ZNET_UNLIKELY_ATTR {
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
    write_cursor_ = buffer.write_cursor_;
    read_cursor_ = buffer.read_cursor_;
    read_limit_ = buffer.read_limit_;
    last_error_ = buffer.last_error_;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = 0;
#endif
    if (allocated_size_ == 0 || !buffer.data_) {
      data_ = nullptr;
      return;
    }
    data_ = new (std::nothrow) char[allocated_size_];
    if (ZNET_UNLIKELY(!data_)) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::CannotAllocate;
      allocated_size_ = 0;
      write_cursor_ = 0;
      read_cursor_ = 0;
      return;
    }
    // only what was written is meaningful; the tail is uninitialized
    std::memcpy(data_, buffer.data_, write_cursor_);
  }

  Buffer& operator=(const Buffer& buffer) {
    if (this != &buffer) {
      Buffer copy(buffer);
      Swap(copy);
    }
    return *this;
  }
#endif

  Buffer(Buffer&& buffer) noexcept
      : endianness_(buffer.endianness_),
        allocated_size_(buffer.allocated_size_),
        write_cursor_(buffer.write_cursor_),
        read_cursor_(buffer.read_cursor_),
        read_limit_(buffer.read_limit_),
        data_(buffer.data_),
        last_error_(buffer.last_error_) {
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    mem_allocations_ = buffer.mem_allocations_;
#endif
    buffer.data_ = nullptr;
    buffer.allocated_size_ = 0;
    buffer.write_cursor_ = 0;
    buffer.read_cursor_ = 0;
  }

  Buffer& operator=(Buffer&& buffer) noexcept {
    if (this != &buffer) {
      delete[] data_;
      data_ = nullptr;
      Swap(buffer);
    }
    return *this;
  }

  // pointer + size is the primary form because std::span is C++20-only; the
  // span overload below is a thin forwarder kept for callers that have one.
  ZNET_TPL_ARITH8(T)
  void Read(T* arr, size_t size) {
    if (ZNET_UNLIKELY(size > std::numeric_limits<size_t>::max() / sizeof(T))) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::CorruptedFormat;
      return;
    }
    char* pt = reinterpret_cast<char*>(arr);
    size_t calculated_size = sizeof(T) * size;
    // bytes, not elements: `size` under-checks by sizeof(T) for anything wider
    // than a char, and the memcpy below reads past the buffer.
    if (ZNET_UNLIKELY(!CheckReadableBytes(calculated_size))) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::ReadOutOfBounds;
      return;
    }
    std::memcpy(pt, data_ + read_cursor_, calculated_size);
    read_cursor_ += calculated_size;
  }

#if ZNET_HAS_CXX20
  ZNET_TPL_ARITH8(T)
  void Read(std::span<T> data) {
    Read(data.data(), data.size());
  }
#endif

  char ReadChar() { return ReadInt<char>(); }

  unsigned char ReadUnsignedChar() { return ReadInt<unsigned char>(); }

  bool ReadBool() { return ReadInt<bool>(); }

  float ReadFloat() { return ReadNumber<float>(); }

  double ReadDouble() { return ReadNumber<double>(); }

  ZNET_TPL_INT16(T)
  T ReadInt() {
    return ReadNumber<T>();
  }

  ZNET_TPL_ARITH16(T)
  T ReadNumber() {
    size_t size = sizeof(T);
    char data[sizeof(T)];
    if (ZNET_UNLIKELY(!CheckReadableBytes(size))) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::ReadOutOfBounds;
      return 0;
    }
    // likely, most systems use native endianness
    if (ZNET_LIKELY(GetSystemEndianness() == endianness_)) ZNET_LIKELY_ATTR {
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
    std::memcpy(&l, data, size);
    return l;
  }

  ZNET_TPL_HAS_READ(T)
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
    // a short read leaves Read() with nothing to copy, and the loop below still
    // runs over every byte
    uint8_t data[BYTES] = {};
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

  ZNET_TPL_ARITH8(T)
  T ReadVarInt() {
    constexpr uint8_t size = sizeof(T);
    char data[size];
    std::memset(data, 0, size);
    if (ZNET_UNLIKELY(!CheckReadableBytes(1))) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::ReadOutOfBounds;
      return 0;
    }
    unsigned char actual_size = ReadUnsignedChar();
    // the count comes off the wire, so a peer could claim more bytes than T
    // holds and walk past the destination.
    if (ZNET_UNLIKELY(actual_size > size)) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::CorruptedFormat;
      return 0;
    }
    if (ZNET_UNLIKELY(!CheckReadableBytes(actual_size))) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::ReadOutOfBounds;
      return 0;
    }
    // likely, most systems use native endianness
    if (ZNET_LIKELY(GetSystemEndianness() == endianness_)) ZNET_LIKELY_ATTR {
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
    if (ZNET_UNLIKELY(!CheckReadCount(size, ZNET_MAX_READ_STRING_LENGTH, 1))) ZNET_UNLIKELY_ATTR {
      return "";
    }
    std::string out(data_ + read_cursor_, size);
    read_cursor_ += size;
    return out;
  }

  template <typename Map, typename KeyFunc, typename ValueFunc>
  Map ReadMap(KeyFunc key_func, ValueFunc value_func) {
    size_t size = ReadVarInt<size_t>();
    // a key and a value, and no reader consumes nothing, so two bytes an entry
    // is the floor.
    if (ZNET_UNLIKELY(!CheckReadCount(size, ZNET_MAX_READ_ELEMENTS, 2))) ZNET_UNLIKELY_ATTR {
      return {};
    }
    Map map;
    for (size_t i = 0; i < size; i++) {
      auto key = (this->*key_func)();
      auto value = (this->*value_func)();
      map[key] = value;
    }
    return map;
  }

  template <typename T, typename ValueFunc>
  std::vector<T> ReadVector(ValueFunc value_func) {
    size_t size = ReadVarInt<size_t>();
    // the reserve below is the reason this is checked before anything else:
    // it is one allocation sized by a stranger.
    if (ZNET_UNLIKELY(!CheckReadCount(size, ZNET_MAX_READ_ELEMENTS, 1))) ZNET_UNLIKELY_ATTR {
      return {};
    }
    std::vector<T> v;
    v.reserve(size);
    for (size_t i = 0; i < size; i++) {
      v.push_back((this->*value_func)());
    }
    return v;
  }

  template <typename T, typename ValueFunc>
  std::unique_ptr<T[]> ReadArray(ValueFunc value_func) {
    size_t size = ReadVarInt<size_t>();
    if (ZNET_UNLIKELY(!CheckReadCount(size, ZNET_MAX_READ_ELEMENTS, sizeof(T)))) ZNET_UNLIKELY_ATTR {
      return nullptr;
    }
    T* ptr = new (std::nothrow) T[size];
    if (ZNET_UNLIKELY(!ptr)) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::CannotAllocate;
      return nullptr;
    }
    std::unique_ptr<T[]> array(ptr);
    for (size_t i = 0; i < size; i++) {
      array[i] = (this->*value_func)();
    }
    return array;
  }

  template <typename T, size_t size, typename ValueFunc>
  std::array<T, size> ReadArray(ValueFunc value_func) {
    size_t size_r = ReadVarInt<size_t>();
    // the count is fixed by the type, so a mismatch is the only thing to catch.
    if (ZNET_UNLIKELY(size_r != size)) ZNET_UNLIKELY_ATTR {
      ZNET_LOG_ERROR("Array size mismatch. Expected: {}, Actual: {}", size,
                     size_r);
      last_error_ = BufferError::CorruptedFormat;
      return {};
    }
    size_t size_bytes = size * sizeof(T);
    if (ZNET_UNLIKELY(!CheckReadableBytes(size_bytes))) ZNET_UNLIKELY_ATTR {
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

  ZNET_TPL_INT16(T)
  void WriteInt(T c) {
    return WriteNumber(c);
  }

  ZNET_TPL_ARITH16(T)
  void WriteNumber(T c) {
    char* pt = reinterpret_cast<char*>(&c);
    size_t size = sizeof(c);
    ReserveIncremental(size);
    // likely, most systems use native endianness
    if (ZNET_LIKELY(GetSystemEndianness() == endianness_)) ZNET_LIKELY_ATTR {
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

  ZNET_TPL_HAS_WRITE(T)
  void WriteCustom(T& ptr) {
    ptr->Write(*this);
  }

  void WriteInetAddress(const InetAddress& address) {
    if (address.ipv() == InetProtocolVersion::IPv4) {
      WriteInt<uint8_t>(4);
      // raw IPv4 (network-order) + port (network-order)
      auto* addr = reinterpret_cast<const sockaddr_in*>(address.handle_ptr());
      Write(reinterpret_cast<const uint32_t*>(&addr->sin_addr.s_addr), 1);
      Write(&addr->sin_port, 1);
    } else if (address.ipv() == InetProtocolVersion::IPv6) {
      WriteInt<uint8_t>(6);
      auto* addr = reinterpret_cast<const sockaddr_in6*>(address.handle_ptr());
      // raw IPv6 (16 bytes) + port
      Write(addr->sin6_addr.s6_addr, 16);
      Write(&addr->sin6_port, 1);
    } else {
      WriteInt<uint8_t>(0);
    }
  }

  void WritePort(PortNumber port) {
    WriteInt(port);
  }

  // write a std::bitset<N> (little-endian bit order)
  template<size_t N>
  void WriteBitset(const std::bitset<N>& bs) {
    constexpr size_t BYTES = (N + 7) / 8;
    uint8_t data[BYTES] = {};
    for (size_t i = 0; i < N; ++i) {
      if (bs[i]) {
        data[i/8] |= uint8_t(1u << (i % 8));
      }
    }
    Write(data, BYTES);
  }

  ZNET_TPL_ARITH8(T)
  void Write(const T* arr, size_t size) {
    auto* pt = reinterpret_cast<const char*>(arr);
    size_t calculated_size = sizeof(T) * size;
    ReserveIncremental(calculated_size);
    std::memcpy(data_ + write_cursor_, pt, calculated_size);
    write_cursor_ += calculated_size;
  }

#if ZNET_HAS_CXX20
  ZNET_TPL_ARITH8(T)
  void Write(std::span<const T> data) { Write(data.data(), data.size()); }
#endif

  ZNET_TPL_ARITH8(T)
  void WriteVarInt(T c) {
    auto* raw_data = reinterpret_cast<const char*>(&c);
    unsigned char size = sizeof(c);  // assume 1 byte for the size
    unsigned char actual_size = 0;
    for (uint8_t i = 0; i < size; i++) {
      if (raw_data[i] != 0) {
        actual_size = i + 1;
      }
    }
    ReserveIncremental(actual_size + 1);
    WriteUnsignedChar(actual_size);
    // likely, most systems use native endianness
    if (ZNET_LIKELY(GetSystemEndianness() == endianness_)) ZNET_LIKELY_ATTR {
      for (size_t i = 0; i < actual_size; i++) {
        data_[write_cursor_ + i] = raw_data[i];
      }
    } else {
      for (size_t i = actual_size, j = 0; i > 0; i--, j++) {
        data_[write_cursor_ + j] = raw_data[i - 1];
      }
    }
    write_cursor_ += actual_size;
  }

  template <typename KeyFunc, typename ValueFunc, typename Map>
  void WriteMap(Map& map, KeyFunc key_func, ValueFunc value_func) {
    WriteVarInt(map.size());
    for (auto& kv : map) {
      (this->*key_func)(kv.first);
      (this->*value_func)(kv.second);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteVector(std::vector<T>& v, ValueFunc value_func) {
    size_t size = v.size();
    WriteVarInt(size);
    for (auto& value : v) {
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T, size_t size>
  void WriteArray(T (&v)[size], ValueFunc value_func) {
    WriteVarInt(size);
    for (size_t i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteArray(T* v, size_t size, ValueFunc value_func) {
    if (!v) {
      WriteVarInt<size_t>(0);
      return;
    }
    WriteVarInt(size);
    for (size_t i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  template <typename ValueFunc, typename T>
  void WriteArray(std::shared_ptr<T[]>& v, size_t size, ValueFunc value_func) {
    WriteVarInt(size);
    for (size_t i = 0; i < size; i++) {
      auto& value = v[i];
      (this->*value_func)(value);
    }
  }

  std::string Dump(int width = 2, size_t wrap = 8) const {
    std::string str;
    for (size_t i = 0; i < write_cursor_; i++) {
      if (i != 0) {
        if (i % wrap == 0) {
          str += "\n";
        } else {
          str += " ";
        }
      }
      str += ToHex(static_cast<uint8_t>(data_[i]), width);
    }
    return str;
  }

  void Trim() {
    // most buffers aren't already exactly trimmed
    if (ZNET_UNLIKELY(write_cursor_ == allocated_size_)) ZNET_UNLIKELY_ATTR {
      return;
    }
    char* new_data = new (std::nothrow) char[write_cursor_];
    if (ZNET_UNLIKELY(!new_data)) ZNET_UNLIKELY_ATTR {
      last_error_ = BufferError::CannotAllocate;
      return;
    }
    std::memcpy(new_data, data_, write_cursor_);
    delete[] data_;
    data_ = new_data;
    allocated_size_ = write_cursor_;
  }

  /**
   * @brief Slides the unread bytes to the front, making the consumed space
   *        writable again.
   *
   * For a stream accumulator: parse from the read cursor, append at the write
   * cursor, and compact between rounds so a fixed reservation never fills up
   * with already-consumed bytes. Never allocates or shrinks.
   */
  void Compact() {
    if (read_cursor_ == 0) {
      return;
    }
    const size_t unread =
        write_cursor_ > read_cursor_ ? write_cursor_ - read_cursor_ : 0;
    if (unread > 0) {
      std::memmove(data_, data_ + read_cursor_, unread);
    }
    read_cursor_ = 0;
    write_cursor_ = unread;
  }

  void Reset(bool deallocate = false) {
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

  ZNET_NODISCARD Endianness endianness() const { return endianness_; }

  ZNET_NODISCARD const char* data() const { return data_; }

  ZNET_NODISCARD const char* read_cursor_data() const {
    return data_ + read_cursor_;
  }

  char* data_mutable() { return data_; }

  /**
   * @brief Where the next write lands.
   *
   * For an external writer (a recv, a cipher, a decompressor) that produces
   * bytes directly into the buffer: reserve the space, write here, then
   * CommitWrite what was actually produced. writable_bytes() is how much fits.
   */
  char* write_cursor_data() { return data_ + write_cursor_; }

  ZNET_NODISCARD size_t write_cursor() const { return write_cursor_; }

  void set_write_cursor(size_t cursor) { write_cursor_ = cursor; }

  void set_read_cursor(size_t cursor) { read_cursor_ = cursor; }

  ZNET_NODISCARD size_t read_cursor() const { return read_cursor_; }

  ZNET_NODISCARD size_t size() const { return write_cursor_; }

  ZNET_NODISCARD size_t capacity() const { return allocated_size_; }

  ZNET_NODISCARD size_t readable_bytes() const {
    auto min_cursor = std::min(write_cursor_, read_limit_);
    if (read_cursor_ > min_cursor) {
      return 0;  // Invalid state, no readable bytes
    }
    return min_cursor - read_cursor_;
  }

  ZNET_NODISCARD size_t writable_bytes() const {
    return allocated_size_ - write_cursor_;
  }

#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
  ZNET_NODISCARD size_t mem_allocations() const { return mem_allocations_; }
#endif

  /**
   * @brief Leaves `bytes` of unwritten space in front of the payload.
   *
   * Lets a later stage add a header with PrependInt8 instead of allocating a
   * second buffer and copying the payload forward to make room. Call on a
   * fresh buffer, before writing anything. Allocates nothing itself; the first
   * write sizes the buffer for the headroom and the payload together.
   */
  void ReserveHeadroom(size_t bytes) {
    read_cursor_ = bytes;
    write_cursor_ = bytes;
  }

  /**
   * @brief Writes one byte in front of the read cursor.
   *
   * Spends a byte of what ReserveHeadroom set aside. Returns false when none is
   * left, leaving the buffer untouched so the caller can build a new one.
   */
  bool PrependInt8(uint8_t value) {
    // ReserveHeadroom does not allocate, so data_ is null until the first write
    if (read_cursor_ == 0 || ZNET_UNLIKELY(!data_)) {
      return false;
    }
    data_[--read_cursor_] = static_cast<char>(value);
    return true;
  }

  void SkipRead(size_t size) { read_cursor_ += size; }

  /**
   * @brief Advances the write cursor over bytes an external writer already
   *        placed at write_cursor_data().
   *
   * Unlike SkipWrite this never allocates: the bytes exist, so their space
   * was necessarily reserved before they were written.
   */
  void CommitWrite(size_t bytes) {
#if defined(DEBUG)
    assert(bytes <= writable_bytes());
#endif
    write_cursor_ += bytes;
  }

  void SetReadLimit(size_t limit) {
    if (limit == 0) {
      read_limit_ = std::numeric_limits<size_t>::max();
    } else {
      read_limit_ = limit;
    }
  }

  void SkipWrite(size_t size) {
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
    // growth floor: doubling from the requested size alone makes a buffer
    // filled a few bytes at a time crawl through 2, 6, 14... byte
    // reallocations, one per write. exact reservations are left exact.
    constexpr size_t kMinGrowth = 64;
    if (ZNET_UNLIKELY(!data_)) ZNET_UNLIKELY_ATTR {
      size_t target_size;
      if (exact) {
        target_size = size;
      } else {
        target_size = size * 2;
        if (target_size < kMinGrowth) {
          target_size = kMinGrowth;
        }
      }
      data_ = new (std::nothrow) char[target_size];
      if (ZNET_UNLIKELY(!data_)) ZNET_UNLIKELY_ATTR {
        last_error_ = BufferError::CannotAllocate;
        return;
      }
      allocated_size_ = target_size;
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
      mem_allocations_++;
#endif
      return;
    }
    // most Reserve calls don't need to reallocate
    if (ZNET_LIKELY(allocated_size_ >= size)) ZNET_LIKELY_ATTR {
      return;
    }
    size_t target_size_ = size * 2;
    if (target_size_ < kMinGrowth) {
      target_size_ = kMinGrowth;
    }
    char* tmp_data = new (std::nothrow) char[target_size_];
    if (ZNET_UNLIKELY(!tmp_data)) ZNET_UNLIKELY_ATTR {
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

 private:
  void Swap(Buffer& other) noexcept {
    std::swap(endianness_, other.endianness_);
    std::swap(allocated_size_, other.allocated_size_);
    std::swap(write_cursor_, other.write_cursor_);
    std::swap(read_cursor_, other.read_cursor_);
    std::swap(read_limit_, other.read_limit_);
    std::swap(data_, other.data_);
    std::swap(last_error_, other.last_error_);
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
    std::swap(mem_allocations_, other.mem_allocations_);
#endif
  }

  ZNET_NODISCARD bool CheckReadableBytes(size_t required) const {
#if defined(DEBUG) && !defined(DISABLE_ASSERT_READABLE_BYTES)
    assert(std::min(write_cursor_, read_limit_) >= read_cursor_ + required);
#endif
    return std::min(write_cursor_, read_limit_) >= read_cursor_ + required;
  }

  /**
   * @brief Whether a length read off the wire may be acted on.
   *
   * Two questions, and a count has to survive both. Is it under the configured
   * ceiling, if one is set -- failing that is @ref BufferError::ReadLimitExceeded.
   * And could the bytes still in the buffer actually back it: every element
   * costs at least @p min_bytes_per_element, so a count above what remains is
   * out of bounds however generous the ceiling is. The second check is the one
   * that makes a nine-byte packet claiming four billion elements cheap to
   * refuse, and it holds whatever the ceiling is set to.
   */
  ZNET_NODISCARD bool CheckReadCount(size_t count, size_t max_allowed,
                                     size_t min_bytes_per_element) {
    if (max_allowed != 0 && count > max_allowed) {
      last_error_ = BufferError::ReadLimitExceeded;
      return false;
    }
    // division, not multiplication: count * per_element is exactly the product
    // an attacker would pick to wrap.
    const size_t per_element =
        min_bytes_per_element == 0 ? 1 : min_bytes_per_element;
    if (count > readable_bytes() / per_element) {
      last_error_ = BufferError::ReadOutOfBounds;
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
#ifdef ZNET_BUFFER_COUNT_MEMORY_ALLOCATIONS
  size_t mem_allocations_;
#endif
};
}  // namespace znet

#endif  // ZNET_BUFFER_H_
