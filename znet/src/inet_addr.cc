//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/inet_addr.h"
#include "znet/logger.h"
#include "znet/init.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace znet {

IPv4Address ParseIPv4(const std::string& ip_str) {
  in_addr addr{};
  inet_pton(AF_INET, ip_str.c_str(), &addr);
  return addr;
}

IPv6Address ParseIPv6(const std::string& ip_str) {
  in6_addr addr{};
  inet_pton(AF_INET6, ip_str.c_str(), &addr);
  return addr;
}

int GetDomainByInetProtocolVersion(InetProtocolVersion version) {
  switch (version) {
    case InetProtocolVersion::IPv4:
      return AF_INET;
    case InetProtocolVersion::IPv6:
      return AF_INET6;
    case InetProtocolVersion::Unix:
#if ZNET_HAS_AF_UNIX
      return AF_UNIX;
#else
      break;
#endif
  }
#if defined(DEBUG) && !defined(DISABLE_ASSERT_INVALID_ADDRESS_PROTOCOL)
  throw std::runtime_error("ipv not supported!");
#endif
  return 0;
}

std::string GetAnyBindAddress(InetProtocolVersion version) {
  switch (version) {
    case InetProtocolVersion::IPv4:
      return "0.0.0.0";
    case InetProtocolVersion::IPv6:
      return "::";
    default:
      ZNET_LOG_ERROR("Invalid InetProtocolVersion: {}", static_cast<int>(version));
      return "0.0.0.0";  // Fallback to IPv4
  }
}


std::string GetLocalAddress(InetProtocolVersion version) {
  switch (version) {
    case InetProtocolVersion::IPv4:
      return "127.0.0.1";
    case InetProtocolVersion::IPv6:
      return "::1";
    default:
      ZNET_LOG_ERROR("Invalid InetProtocolVersion: {}", static_cast<int>(version));
      return "127.0.0.1";  // Fallback to IPv4
  }
}

bool IsIPv4(const std::string& ip) {
  sockaddr_in sa;
  return inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) == 1;
  /*static std::regex ipv4_regex(
      R"(^(?:(?:25[0-5]|2[0-4]\d|1\d{2}|[1-9]?\d)(?:\.|$)){4}$)");
  return std::regex_match(ip, ipv4_regex);*/
}

bool IsIPv6(const std::string& ip) {
  sockaddr_in6 sa6;
  return inet_pton(AF_INET6, ip.c_str(), &(sa6.sin6_addr)) == 1;
  /*static std::regex ipv6_regex(R"(^(([0-9a-fA-F]{1,4}:){1,7}[0-9a-fA-F]{1,4}|::1|::)$)");
  return std::regex_match(ip, ipv6_regex);*/
}

std::string ResolveHostnameToIP(const std::string& hostname) {
  Result init_result = znet::Init();
  if (init_result != Result::Success) {
    ZNET_LOG_WARN("Cannot resolve hostname because initialization of znet had failed with reason: {}", GetResultString(init_result));
    return hostname;
  }
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
  if (err != 0 || !res) {
    ZNET_LOG_WARN("Failed to resolve hostname: {} ({})", hostname, gai_strerror(err));
    return hostname;
  }

  char ip_str[INET6_ADDRSTRLEN] = {};
  const addrinfo* selected = nullptr;

#if ZNET_PREFER_IPV4
  // look for first AF_INET (IPv4)
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    if (p->ai_family == AF_INET) {
      selected = p;
      break;
    }
  }
#endif

  // fallback to the first result
  if (!selected) {
    selected = res;
  }

  void* addr = nullptr;
  if (selected->ai_family == AF_INET) {
    addr = &reinterpret_cast<sockaddr_in*>(selected->ai_addr)->sin_addr;
  } else if (selected->ai_family == AF_INET6) {
    addr = &reinterpret_cast<sockaddr_in6*>(selected->ai_addr)->sin6_addr;
  }

  const char* result = nullptr;
  if (addr) {
    result = inet_ntop(selected->ai_family, addr, ip_str, sizeof(ip_str));
  }

  freeaddrinfo(res);
  if (!result) {
    return hostname;
  }
  return ip_str;
}

std::unique_ptr<InetAddress> InetAddress::from(const std::string& host, PortNumber port) {
  if (host.empty()) {
    return std::make_unique<InetAddressIPv4>(port);
  }
  if (host.compare(0, 5, "unix:") == 0) {
#if ZNET_HAS_AF_UNIX
    (void)port;  // paths have no ports
    return std::make_unique<InetAddressUnix>(host.substr(5));
#else
    ZNET_LOG_ERROR("Unix sockets are not supported on this platform: {}", host);
    return nullptr;
#endif
  }
  std::string ip_str = ResolveHostnameToIP(host);
  if (ip_str == "localhost") {
    // the resolver hands this one back unresolved
    ip_str = "127.0.0.1";
  }
  if (IsIPv4(ip_str)) {
    return std::make_unique<InetAddressIPv4>(ip_str, port);
  } else if (IsIPv6(ip_str)) {
    return std::make_unique<InetAddressIPv6>(ip_str, port);
  }
  ZNET_LOG_WARN("Invalid IP address: {}", ip_str);
  return nullptr;
}

std::unique_ptr<InetAddress> InetAddress::from(sockaddr* sock_addr) {
  if (sock_addr->sa_family == AF_INET) {
    auto* addr = reinterpret_cast<sockaddr_in*>(sock_addr);
    return std::make_unique<InetAddressIPv4>(addr->sin_addr, ntohs(addr->sin_port));
  } else if (sock_addr->sa_family == AF_INET6) {
    auto* addr = reinterpret_cast<sockaddr_in6*>(sock_addr);
    return std::make_unique<InetAddressIPv6>(addr->sin6_addr, ntohs(addr->sin6_port));
  }
#if ZNET_HAS_AF_UNIX
  if (sock_addr->sa_family == AF_UNIX) {
    auto* addr = reinterpret_cast<sockaddr_un*>(sock_addr);
    // a connected client is usually unnamed: the kernel zero-fills sun_path,
    // which reads back here as the empty path
    return std::make_unique<InetAddressUnix>(std::string(addr->sun_path));
  }
#endif

#if defined(DEBUG) && !defined(DISABLE_ASSERT_INVALID_ADDRESS_FAMILY)
  throw std::runtime_error("sockaddr family is not supported");
#endif
  return nullptr;
}

InetAddressIPv4::InetAddressIPv4(PortNumber port)
    : InetAddress(InetProtocolVersion::IPv4, "") {
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(port);
#ifdef ZNET_TARGET_APPLE
  addr_.sin_len = sizeof(sockaddr_in);
#endif
  char src[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr_.sin_addr, src, INET_ADDRSTRLEN);
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr_.sin_port));
  is_valid_ = true;
}

InetAddressIPv4::InetAddressIPv4(IPv4Address ip, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv4, "") {
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(port);
  addr_.sin_addr = ip;
#ifdef ZNET_TARGET_APPLE
  addr_.sin_len = sizeof(sockaddr_in);
#endif

  char src[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr_.sin_addr, src, INET_ADDRSTRLEN);
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr_.sin_port));
  is_valid_ = true;
}

InetAddressIPv4::InetAddressIPv4(const std::string& str, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv4, "") {
  if (!IsIPv4(str)) {
    is_valid_ = false;
    readable_ = "Invalid Address";
    return;
  }

  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(port);
  addr_.sin_addr = ParseIPv4(str);
#ifdef ZNET_TARGET_APPLE
  addr_.sin_len = sizeof(sockaddr_in);
#endif
  char src[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr_.sin_addr, src, INET_ADDRSTRLEN);
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr_.sin_port));
  is_valid_ = true;
}

InetAddressIPv6::InetAddressIPv6(PortNumber port)
    : InetAddress(InetProtocolVersion::IPv6, "") {
  addr_.sin6_family = AF_INET6;
  addr_.sin6_flowinfo = 0;
  addr_.sin6_port = htons(port);
#if !defined(ZNET_TARGET_WIN) && !defined(ZNET_TARGET_WEB) && !defined(ZNET_TARGET_LINUX)
  addr_.sin6_len = sizeof(sockaddr_in6);
#endif
  char src[INET6_ADDRSTRLEN];
  inet_ntop(AF_INET6, &addr_.sin6_addr, src, sizeof(src));
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr_.sin6_port));
  is_valid_ = true;
}

InetAddressIPv6::InetAddressIPv6(IPv6Address ip, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv6, "") {
  addr_.sin6_family = AF_INET6;
  addr_.sin6_flowinfo = 0;
  addr_.sin6_port = htons(port);
  addr_.sin6_addr = ip;
#if !defined(ZNET_TARGET_WIN) && !defined(ZNET_TARGET_WEB) && !defined(ZNET_TARGET_LINUX)
  addr_.sin6_len = sizeof(sockaddr_in6);
#endif
  char src[INET6_ADDRSTRLEN];
  inet_ntop(AF_INET6, &addr_.sin6_addr, src, sizeof(src));
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr_.sin6_port));
  is_valid_ = true;
}

InetAddressIPv6::InetAddressIPv6(const std::string& str, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv6, "") {
  if (!IsIPv6(str)) {
    is_valid_ = false;
    readable_ = "Invalid Address";
    return;
  }
#if !defined(ZNET_TARGET_WIN) && !defined(ZNET_TARGET_WEB) && !defined(ZNET_TARGET_LINUX)
  addr_.sin6_len = sizeof(sockaddr_in6);
#endif
  addr_.sin6_family = AF_INET6;
  addr_.sin6_flowinfo = 0;
  addr_.sin6_port = htons(port);
  addr_.sin6_addr = ParseIPv6(str);
  char src[INET6_ADDRSTRLEN];
  inet_ntop(AF_INET6, &addr_.sin6_addr, src, sizeof(src));
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr_.sin6_port));
  is_valid_ = true;
}

#if ZNET_HAS_AF_UNIX
InetAddressUnix::InetAddressUnix(const std::string& path)
    : InetAddress(InetProtocolVersion::Unix, "") {
  if (path.size() >= sizeof(addr_.sun_path)) {
    ZNET_LOG_WARN("Unix socket path is too long ({} bytes, limit {}): {}",
                  path.size(), sizeof(addr_.sun_path) - 1, path);
    readable_ = "Invalid Address";
    return;
  }
  addr_.sun_family = AF_UNIX;
  std::memcpy(addr_.sun_path, path.data(), path.size());
  addr_.sun_path[path.size()] = '\0';
  addr_len_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                     path.size() + 1);
#ifdef ZNET_TARGET_APPLE
  addr_.sun_len = static_cast<unsigned char>(addr_len_);
#endif
  readable_ = "unix:" + path;
  is_valid_ = true;
}

std::unique_ptr<InetAddress> InetAddressUnix::WithPort(PortNumber port) const {
  (void)port;  // paths have no ports
  return std::make_unique<InetAddressUnix>(std::string(addr_.sun_path));
}
#endif  // ZNET_HAS_AF_UNIX

namespace {

bool PrefixMatch(const unsigned char* a, const unsigned char* b, uint8_t bits) {
  const size_t whole = bits / 8u;
  if (whole != 0 && std::memcmp(a, b, whole) != 0) {
    return false;
  }
  const uint8_t rem = bits % 8u;
  if (rem == 0) {
    return true;
  }
  const unsigned char mask = static_cast<unsigned char>(0xFF << (8 - rem));
  return (a[whole] & mask) == (b[whole] & mask);
}

// true when `bytes` is ::ffff:a.b.c.d, the shape a v4 client takes on a
// dual-stack socket
bool IsV4MappedV6(const unsigned char* bytes) {
  for (int i = 0; i < 10; i++) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return bytes[10] == 0xFF && bytes[11] == 0xFF;
}

}  // namespace

// below the helpers rather than with its siblings: the mapped-v4 collapse is
// this file's private business
std::string InetAddressIPv6::host_key() const {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&addr_.sin6_addr);
  if (IsV4MappedV6(bytes)) {
    return std::string(reinterpret_cast<const char*>(bytes + 12), 4);
  }
  return std::string(reinterpret_cast<const char*>(bytes), 16);
}

CIDRBlock CIDRBlock::Parse(const std::string& text) {
  CIDRBlock block;
  block.text_ = text;
  std::string addr = text;
  long prefix = -1;
  const size_t slash = text.find('/');
  if (slash != std::string::npos) {
    addr = text.substr(0, slash);
    const std::string prefix_str = text.substr(slash + 1);
    // digits only and short, so strtol cannot be surprised
    if (prefix_str.empty() || prefix_str.size() > 3 ||
        prefix_str.find_first_not_of("0123456789") != std::string::npos) {
      return block;
    }
    prefix = std::strtol(prefix_str.c_str(), nullptr, 10);
  }
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, addr.c_str(), &v4) == 1) {
    std::memcpy(block.bytes_, &v4, 4);
    block.is_ipv6_ = false;
    if (prefix < 0) {
      prefix = 32;  // a bare host is the /32 it names
    }
    if (prefix > 32) {
      return block;
    }
  } else if (inet_pton(AF_INET6, addr.c_str(), &v6) == 1) {
    std::memcpy(block.bytes_, &v6, 16);
    block.is_ipv6_ = true;
    if (prefix < 0) {
      prefix = 128;
    }
    if (prefix > 128) {
      return block;
    }
  } else {
    return block;
  }
  block.prefix_ = static_cast<uint8_t>(prefix);
  block.is_valid_ = true;
  return block;
}

bool CIDRBlock::Matches(const InetAddress& address) const {
  if (!is_valid_) {
    return false;
  }
  const std::string candidate = address.host_key();
  if (candidate.empty()) {
    return false;  // no address to speak of, e.g. a unix path
  }
  if ((candidate.size() == 16) != is_ipv6_) {
    return false;
  }
  return PrefixMatch(bytes_,
                     reinterpret_cast<const unsigned char*>(candidate.data()),
                     prefix_);
}
}  // namespace znet