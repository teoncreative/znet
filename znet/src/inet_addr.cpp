//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/base/inet_addr.h"

namespace znet {

IPv4Type ParseIPv4(const std::string& ip_str) {
  sockaddr_in antelope{};
  inet_pton(AF_INET, ip_str.c_str(), &(antelope.sin_addr));
#ifdef TARGET_WIN
  return antelope.sin_addr;
#else
  return antelope.sin_addr.s_addr;
#endif
}

IPv6Type ParseIPv6(const std::string& ip_str) {
  sockaddr_in6 antelope{};
  inet_pton(AF_INET6, ip_str.c_str(), &(antelope.sin6_addr));
  return antelope.sin6_addr;
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

Scope<InetAddress> InetAddress::from(const std::string& ip_str, PortType port) {
  if (ip_str.empty()) {
    return CreateScope<InetAddressIPv4>("0.0.0.0", port);
  }
  return CreateScope<InetAddressIPv4>(ip_str, port);
}

Scope<InetAddress> InetAddress::from(sockaddr* sock_addr) {
  if (sock_addr->sa_family == AF_INET) {
    auto* addr = (sockaddr_in*)sock_addr;
#ifdef TARGET_WIN
    return CreateRef<InetAddressIPv4>(addr->sin_addr, addr->sin_port);
#else
    return CreateScope<InetAddressIPv4>(addr->sin_addr.s_addr, addr->sin_port);
#endif
  } else if (sock_addr->sa_family == AF_INET6) {
    auto* addr = (sockaddr_in6*)sock_addr;
    return CreateScope<InetAddressIPv6>(addr->sin6_addr, addr->sin6_port);
  }

#if defined(DEBUG) && !defined(DISABLE_ASSERT_INVALID_ADDRESS_FAMILY)
  throw std::runtime_error("sockaddr family is not supported");
#endif
  return nullptr;
}

}  // namespace znet