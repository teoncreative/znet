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

std::string GetAnyBindAddress(InetProtocolVersion version) {
  switch (version) {
    case InetProtocolVersion::IPv4:
      return "0.0.0.0";
    case InetProtocolVersion::IPv6:
      return "::";
  }
}


std::string GetLocalAddress(InetProtocolVersion version) {
  switch (version) {
    case InetProtocolVersion::IPv4:
      return "127.0.0.1";
    case InetProtocolVersion::IPv6:
      return "::1";
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

#ifdef ZNET_PREFER_IPV4
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
  std::string ip_str = ResolveHostnameToIP(host);
  if (ip_str == "localhost") {
    // For some dumb reason
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
    auto* addr = (sockaddr_in*)sock_addr;
    return std::make_unique<InetAddressIPv4>(addr->sin_addr, ntohs(addr->sin_port));
  } else if (sock_addr->sa_family == AF_INET6) {
    auto* addr = (sockaddr_in6*)sock_addr;
    return std::make_unique<InetAddressIPv6>(addr->sin6_addr, ntohs(addr->sin6_port));
  }

#if defined(DEBUG) && !defined(DISABLE_ASSERT_INVALID_ADDRESS_FAMILY)
  throw std::runtime_error("sockaddr family is not supported");
#endif
  return nullptr;
}

InetAddressIPv4::InetAddressIPv4(PortNumber port)
    : InetAddress(InetProtocolVersion::IPv4, "") {
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(port);
#ifdef TARGET_APPLE
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
#ifdef TARGET_APPLE
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
#ifdef TARGET_APPLE
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
#if !defined(TARGET_WIN) && !defined(TARGET_WEB) && !defined(TARGET_LINUX)
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
#if !defined(TARGET_WIN) && !defined(TARGET_WEB) && !defined(TARGET_LINUX)
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
#if !defined(TARGET_WIN) && !defined(TARGET_WEB) && !defined(TARGET_LINUX)
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
}  // namespace znet