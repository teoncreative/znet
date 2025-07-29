//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "types.h"

#pragma once

namespace znet {

#if defined(TARGET_APPLE) || defined(TARGET_WEB) || defined(TARGET_LINUX)
using SocketHandle = int;
using PortNumber = in_port_t;
using IPv4Address = in_addr;
using IPv6Address = in6_addr;
#elif defined(TARGET_WIN)
using SocketHandle = SOCKET;
using PortNumber = USHORT;
using IPv4Address = IN_ADDR;
using IPv6Address = IN6_ADDR;
#endif

enum class InetProtocolVersion { IPv4, IPv6 };

IPv4Address ParseIPv4(const std::string& ip_str);
IPv6Address ParseIPv6(const std::string& ip_str);

int GetDomainByInetProtocolVersion(InetProtocolVersion version);

bool IsIPv4(const std::string& ip);
bool IsIPv6(const std::string& ip);

bool IsValidIPv4(const std::string& ip);
bool IsValidIPv6(const std::string& ip);

class InetAddress {
 public:
  InetAddress(InetProtocolVersion ipv, std::string readable)
      : ipv_(ipv), readable_(std::move(readable)) {}

  operator bool() const { return is_valid(); }

  ZNET_NODISCARD virtual bool is_valid() const { return false; }

  ZNET_NODISCARD virtual const std::string& readable() const {
    return readable_;
  };

  ZNET_NODISCARD InetProtocolVersion ipv() const { return ipv_; }

  ZNET_NODISCARD virtual socklen_t addr_size() const { return 0; }

  ZNET_NODISCARD virtual sockaddr* handle_ptr() const { return nullptr; }

  static std::unique_ptr<InetAddress> from(const std::string& ip_str, PortNumber port);
  static std::unique_ptr<InetAddress> from(sockaddr* addr);

 protected:
  InetProtocolVersion ipv_;
  std::string readable_;
};

class InetAddressIPv4 : public InetAddress {
 public:
  explicit InetAddressIPv4(PortNumber port)
      : InetAddress(InetProtocolVersion::IPv4, "") {
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef TARGET_APPLE
    addr.sin_len = sizeof(sockaddr_in);
#endif
    char src[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, src, INET_ADDRSTRLEN);
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin_port));
    is_valid_ = true;
  }

  InetAddressIPv4(IPv4Address ip, PortNumber port)
      : InetAddress(InetProtocolVersion::IPv4, "") {
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ip;
#ifdef TARGET_APPLE
    addr.sin_len = sizeof(sockaddr_in);
#endif

    char src[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, src, INET_ADDRSTRLEN);
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin_port));
    is_valid_ = true;
  }

  InetAddressIPv4(const std::string& str, PortNumber port)
      : InetAddress(InetProtocolVersion::IPv4, "") {
    if (!IsValidIPv4(str)) {
      is_valid_ = false;
      readable_ = "Invalid Address";
      return;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ParseIPv4(str);
#ifdef TARGET_APPLE
    addr.sin_len = sizeof(sockaddr_in);
#endif
    char src[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, src, INET_ADDRSTRLEN);
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin_port));
    is_valid_ = true;
  }

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD socklen_t addr_size() const override { return sizeof(addr); }

  ZNET_NODISCARD sockaddr* handle_ptr() const override {
    return (sockaddr*)&addr;
  }

 private:
  sockaddr_in addr{};
  bool is_valid_;
};

class InetAddressIPv6 : public InetAddress {
 public:
  explicit InetAddressIPv6(PortNumber port)
      : InetAddress(InetProtocolVersion::IPv6, "") {
    addr.sin6_family = AF_INET6;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port);
#if !defined(TARGET_WIN) && !defined(TARGET_WEB) && !defined(TARGET_LINUX)
    addr.sin6_len = sizeof(sockaddr_in6);
#endif
    char src[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr.sin6_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin6_port));
    is_valid_ = true;
  }

  InetAddressIPv6(IPv6Address ip, PortNumber port)
      : InetAddress(InetProtocolVersion::IPv6, "") {
    addr.sin6_family = AF_INET6;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port);
    addr.sin6_addr = ip;
#if !defined(TARGET_WIN) && !defined(TARGET_WEB) && !defined(TARGET_LINUX)
    addr.sin6_len = sizeof(sockaddr_in6);
#endif
    char src[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr.sin6_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin6_port));
    is_valid_ = true;
  }

  InetAddressIPv6(const std::string& str, PortNumber port)
      : InetAddress(InetProtocolVersion::IPv6, "") {
    if (!IsValidIPv6(str)) {
      is_valid_ = false;
      readable_ = "Invalid Address";
      return;
    }
#if !defined(TARGET_WIN) && !defined(TARGET_WEB) && !defined(TARGET_LINUX)
    addr.sin6_len = sizeof(sockaddr_in6);
#endif
    addr.sin6_family = AF_INET6;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port);
    addr.sin6_addr = ParseIPv6(str);
    char src[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr.sin6_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin6_port));
    is_valid_ = true;
  }

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD socklen_t addr_size() const override { return sizeof(addr); }

  ZNET_NODISCARD sockaddr* handle_ptr() const override {
    return (sockaddr*)&addr;
  }

 private:
  sockaddr_in6 addr{};
  bool is_valid_;
};

}  // namespace znet