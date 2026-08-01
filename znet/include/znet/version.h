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
// znet's version. This header is the single source of truth: CMake parses the
// numbers below for project(znet VERSION ...), so the two cannot drift, and the
// header stays usable on its own when the headers are vendored into a build that
// does not run znet's CMake.
//
// The macros describe the headers you compiled against; the functions describe
// the library you linked. They differ when a binary is older than its headers,
// which is worth checking if you ship znet as a shared library.
//
//   static_assert(ZNET_VERSION >= ZNET_MAKE_VERSION(3, 1, 0));
//   ZNET_LOG_INFO("znet {}", znet::VersionString());
//

#ifndef ZNET_PARENT_VERSION_H
#define ZNET_PARENT_VERSION_H

#define ZNET_VERSION_MAJOR 3
#define ZNET_VERSION_MINOR 2
#define ZNET_VERSION_PATCH 0

/**
 * @brief Encodes a version as a single comparable integer.
 *
 * Lets versions be tested with the usual relational operators in #if.
 */
#define ZNET_MAKE_VERSION(major, minor, patch) \
  ((major) * 1000000 + (minor) * 1000 + (patch))

#define ZNET_VERSION \
  ZNET_MAKE_VERSION(ZNET_VERSION_MAJOR, ZNET_VERSION_MINOR, ZNET_VERSION_PATCH)

#define ZNET_VERSION_STRINGIFY_(x) #x
#define ZNET_VERSION_STRINGIFY(x) ZNET_VERSION_STRINGIFY_(x)

#define ZNET_VERSION_STRING                  \
  ZNET_VERSION_STRINGIFY(ZNET_VERSION_MAJOR) \
  "." ZNET_VERSION_STRINGIFY(ZNET_VERSION_MINOR) \
  "." ZNET_VERSION_STRINGIFY(ZNET_VERSION_PATCH)

namespace znet {

/** @brief Version of the linked library, in ZNET_MAKE_VERSION form. */
int VersionNumber();

/** @brief Version of the linked library as "major.minor.patch". */
const char* VersionString();

}  // namespace znet

#endif  // ZNET_PARENT_VERSION_H
