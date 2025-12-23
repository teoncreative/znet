//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#pragma once

#include "znet/precompiled.h"
#include "znet/types.h"

namespace znet {

enum class InetProtocolVersion { IPv4, IPv6 };

std::string ResolveHostnameToIP(const std::string& hostname);

IPv4Address ParseIPv4(const std::string& ip_str);
IPv6Address ParseIPv6(const std::string& ip_str);

int GetDomainByInetProtocolVersion(InetProtocolVersion version);

std::string GetAnyBindAddress(InetProtocolVersion version);
std::string GetLocalAddress(InetProtocolVersion version);

bool IsIPv4(const std::string& ip);
bool IsIPv6(const std::string& ip);

class InetAddress {
 public:
  InetAddress(InetProtocolVersion ipv, std::string readable)
      : ipv_(ipv), readable_(std::move(readable)) {}
  virtual ~InetAddress() = default;

  operator bool() const { return is_valid(); }

  ZNET_NODISCARD virtual bool is_valid() const { return false; }

  ZNET_NODISCARD virtual const std::string& readable() const {
    return readable_;
  };

  ZNET_NODISCARD InetProtocolVersion ipv() const { return ipv_; }

  ZNET_NODISCARD virtual socklen_t addr_size() const = 0;

  ZNET_NODISCARD virtual sockaddr* handle_ptr() const = 0;

  ZNET_NODISCARD virtual PortNumber port() const = 0;

  ZNET_NODISCARD virtual std::unique_ptr<InetAddress> WithPort(PortNumber port) const = 0;

  static std::unique_ptr<InetAddress> from(const std::string& host, PortNumber port);
  static std::unique_ptr<InetAddress> from(sockaddr* addr);

 protected:
  InetProtocolVersion ipv_;
  std::string readable_;
};

class InetAddressIPv4 : public InetAddress {
 public:
  explicit InetAddressIPv4(PortNumber port);

  InetAddressIPv4(IPv4Address ip, PortNumber port);

  InetAddressIPv4(const std::string& str, PortNumber port);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD socklen_t addr_size() const override { return sizeof(addr_); }

  ZNET_NODISCARD sockaddr* handle_ptr() const override {
    return (sockaddr*)&addr_;
  }

  ZNET_NODISCARD PortNumber port() const override {
    return ntohs(addr_.sin_port);
  }

  ZNET_NODISCARD std::unique_ptr<InetAddress> WithPort(PortNumber port) const override {
    return std::make_unique<InetAddressIPv4>(addr_.sin_addr, port);
  }

 private:
  sockaddr_in addr_{};
  bool is_valid_;
};

class InetAddressIPv6 : public InetAddress {
 public:
  explicit InetAddressIPv6(PortNumber port);
  InetAddressIPv6(IPv6Address ip, PortNumber port);
  InetAddressIPv6(const std::string& str, PortNumber port);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD socklen_t addr_size() const override { return sizeof(addr_); }

  ZNET_NODISCARD sockaddr* handle_ptr() const override {
    return (sockaddr*)&addr_;
  }

  ZNET_NODISCARD PortNumber port() const override {
    return ntohs(addr_.sin6_port);
  }

  ZNET_NODISCARD std::unique_ptr<InetAddress> WithPort(PortNumber port) const override {
    return std::make_unique<InetAddressIPv6>(addr_.sin6_addr, port);
  }
 private:
  sockaddr_in6 addr_{};
  bool is_valid_;
};

}  // namespace znet