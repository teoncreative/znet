//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "base/inet_addr.h"

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
  }
#if defined(DEBUG) && !defined(DISABLE_ASSERT_INVALID_ADDRESS_PROTOCOL)
  throw std::runtime_error("ipv not supported!");
#endif
  return 0;
}

bool IsIPv4(const std::string& ip) {
  static std::regex ipv4_regex(
      "^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(25[0-5]|2[0-4][0-9]|[01]"
      "?[0-9][0-9]?)$");
  return std::regex_match(ip, ipv4_regex);
}

bool IsIPv6(const std::string& ip) {
  static std::regex ipv6_regex("^([0-9a-fA-F]{1,4}:){7}([0-9a-fA-F]{1,4})$");
  return std::regex_match(ip, ipv6_regex);
}

bool IsValidIPv4(const std::string& ip) {
  in_addr addr{};
  return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool IsValidIPv6(const std::string& ip) {
  in6_addr addr{};
  return inet_pton(AF_INET6, ip.c_str(), &addr) == 1;
}

std::unique_ptr<InetAddress> InetAddress::from(const std::string& ip_str, PortNumber port) {
  if (ip_str.empty() || ip_str == "localhost") {
#ifdef ZNET_DEFAULT_IPV6
    return std::make_unique<InetAddressIPv6>(port);
#else
    return std::make_unique<InetAddressIPv4>(port);
#endif
  }
  if (IsIPv4(ip_str)) {
    return std::make_unique<InetAddressIPv4>(ip_str, port);
  } else if (IsIPv6(ip_str)) {
    return std::make_unique<InetAddressIPv6>(ip_str, port);
  }
  return nullptr;
}

std::unique_ptr<InetAddress> InetAddress::from(sockaddr* sock_addr) {
  if (sock_addr->sa_family == AF_INET) {
    auto* addr = (sockaddr_in*)sock_addr;
    return std::make_unique<InetAddressIPv4>(addr->sin_addr, addr->sin_port);
  } else if (sock_addr->sa_family == AF_INET6) {
    auto* addr = (sockaddr_in6*)sock_addr;
    return std::make_unique<InetAddressIPv6>(addr->sin6_addr, addr->sin6_port);
  }

#if defined(DEBUG) && !defined(DISABLE_ASSERT_INVALID_ADDRESS_FAMILY)
  throw std::runtime_error("sockaddr family is not supported");
#endif
  return nullptr;
}

}  // namespace znet