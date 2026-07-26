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
// A minimal std::format stand-in for pre-C++20 builds.
//
// This is only ever reached by the logging macros, and only when neither
// <format> nor fmtlib is available, so it optimises for having no dependency
// rather than for speed. It understands `{}`, the `{{`/`}}` escapes, and it
// tolerates (by ignoring) a format spec such as `{:.2f}` so that a call site
// written for std::format still produces readable output here instead of
// failing to compile.
//
// Where the two can disagree, this follows std::format rather than iostreams:
// bool prints as true/false, and the char-sized integer types print as numbers.
//
// Define ZNET_USE_FMTLIB=1 and link fmt::fmt to get full spec support on
// C++14/17.
//

#ifndef ZNET_DETAIL_FORMAT_H
#define ZNET_DETAIL_FORMAT_H

#include <ostream>
#include <sstream>
#include <string>

namespace znet {
namespace detail {

// std::format prints bool as true/false and the char-sized integers as
// numbers; operator<< would print 1/0 and a raw byte. Match std::format so log
// output does not change with the language level.
inline void FormatArg(std::ostringstream& os, bool v) {
  os << (v ? "true" : "false");
}
inline void FormatArg(std::ostringstream& os, signed char v) {
  os << static_cast<int>(v);
}
inline void FormatArg(std::ostringstream& os, unsigned char v) {
  os << static_cast<unsigned int>(v);
}
template <typename T>
inline void FormatArg(std::ostringstream& os, const T& v) {
  os << v;
}

/**
 * @brief Copies literal text up to the next placeholder, unescaping braces.
 * @return Position just past the placeholder, or nullptr if the format string
 *         ran out before one was found.
 */
inline const char* FormatUntilPlaceholder(std::ostringstream& os,
                                          const char* p) {
  while (*p != '\0') {
    if (*p == '{') {
      if (p[1] == '{') {
        os << '{';
        p += 2;
        continue;
      }
      // A placeholder. Skip any format spec; this shim cannot honour it.
      const char* q = p + 1;
      while (*q != '\0' && *q != '}') {
        ++q;
      }
      return (*q == '\0') ? q : q + 1;
    }
    if (*p == '}' && p[1] == '}') {
      os << '}';
      p += 2;
      continue;
    }
    os << *p;
    ++p;
  }
  return nullptr;
}

/** @brief Emits the tail of the format string once the arguments run out. */
inline void FormatImpl(std::ostringstream& os, const char* p) {
  while (*p != '\0') {
    if ((*p == '{' && p[1] == '{') || (*p == '}' && p[1] == '}')) {
      os << *p;
      p += 2;
      continue;
    }
    os << *p;
    ++p;
  }
}

template <typename T, typename... Rest>
inline void FormatImpl(std::ostringstream& os, const char* p, const T& value,
                       const Rest&... rest) {
  const char* next = FormatUntilPlaceholder(os, p);
  if (next == nullptr) {
    return;  // more arguments than placeholders; drop the extras
  }
  FormatArg(os, value);
  FormatImpl(os, next, rest...);
}

template <typename... Args>
inline std::string Format(const char* fmt, const Args&... args) {
  std::ostringstream os;
  FormatImpl(os, fmt, args...);
  return os.str();
}

template <typename... Args>
inline std::string Format(const std::string& fmt, const Args&... args) {
  return Format(fmt.c_str(), args...);
}

}  // namespace detail
}  // namespace znet

#endif  // ZNET_DETAIL_FORMAT_H
