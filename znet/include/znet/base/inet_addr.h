//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <utility>
#include "types.h"

#pragma once

namespace znet {

#ifdef TARGET_APPLE
using SocketType = int;
using PortType = in_port_t;
using IPv4Type = in_addr;
using IPv6Type = in6_addr;
#elif defined(TARGET_WIN)
using SocketType = SOCKET;
using PortType = USHORT;
using IPv4Type = IN_ADDR;
using IPv6Type = IN6_ADDR;
#endif

enum class InetProtocolVersion { IPv4, IPv6 };

IPv4Type ParseIPv4(const std::string& ip_str);
IPv6Type ParseIPv6(const std::string& ip_str);

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

  static Scope<InetAddress> from(const std::string& ip_str, PortType port);
  static Scope<InetAddress> from(sockaddr* addr);

 protected:
  InetProtocolVersion ipv_;
  std::string readable_;
};

class InetAddressIPv4 : public InetAddress {
 public:
  explicit InetAddressIPv4(PortType port)
      : InetAddress(InetProtocolVersion::IPv4, "") {
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    char src[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin_port));
    is_valid_ = true;
  }

  InetAddressIPv4(IPv4Type ip, PortType port)
      : InetAddress(InetProtocolVersion::IPv4, "") {
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ip;

    char src[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin_port));
    is_valid_ = true;
  }

  InetAddressIPv4(const std::string& str, PortType port)
      : InetAddress(InetProtocolVersion::IPv4, "") {
    if (!IsValidIPv4(str)) {
      is_valid_ = false;
      readable_ = "Invalid Address";
      return;
    }

    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ParseIPv4(str);
    char src[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, src, sizeof(src));
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
  explicit InetAddressIPv6(PortType port)
      : InetAddress(InetProtocolVersion::IPv6, "") {
#ifndef TARGET_WIN
    addr.sin6_len = sizeof(sockaddr_in6);
#endif
    addr.sin6_family = AF_INET6;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    char src[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr.sin6_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin6_port));
    is_valid_ = true;
  }

  InetAddressIPv6(IPv6Type ip, PortType port)
      : InetAddress(InetProtocolVersion::IPv6, "") {
#ifndef TARGET_WIN
    addr.sin6_len = sizeof(sockaddr_in6);
#endif
    addr.sin6_family = AF_INET6;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port);
    addr.sin6_addr = ip;
    char src[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr.sin6_addr, src, sizeof(src));
    readable_ = std::string(src) + ":" + std::to_string(ntohs(addr.sin6_port));
    is_valid_ = true;
  }

  InetAddressIPv6(const std::string& str, PortType port)
      : InetAddress(InetProtocolVersion::IPv6, "") {
    if (!IsValidIPv6(str)) {
      is_valid_ = false;
      readable_ = "Invalid Address";
      return;
    }
#ifndef TARGET_WIN
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