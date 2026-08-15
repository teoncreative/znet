//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#ifndef ZNET_INET_ADDR_H_
#define ZNET_INET_ADDR_H_

#include "znet/compat.h"
#include "znet/detail/platform.h"
#include "znet/types.h"

#include <cstddef>
#include <memory>
#include <string>

#ifndef ZNET_PREFER_IPV4
#define ZNET_PREFER_IPV4 0
#endif

struct sockaddr;

namespace znet {

enum class InetProtocolVersion { IPv4, IPv6, Unix };

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

  /**
   * @brief The bytes naming the host, without the port: 4 for IPv4, 16 for
   *        IPv6, empty when there are none (unix paths). An IPv4-mapped IPv6
   *        address collapses to the IPv4 it carries, so one host reads the
   *        same through either family. What admission keys and matches on.
   */
  ZNET_NODISCARD virtual std::string host_key() const { return {}; }

  /** @brief What bind() and connect() want alongside handle_ptr(): the whole
   *  struct for IPv4 and IPv6, the used prefix for a unix path. */
  ZNET_NODISCARD SockLen addr_size() const { return addr_len_; }

  ZNET_NODISCARD virtual const sockaddr* handle_ptr() const = 0;

  ZNET_NODISCARD virtual PortNumber port() const = 0;

  ZNET_NODISCARD virtual std::unique_ptr<InetAddress> WithPort(PortNumber port) const = 0;

  static std::unique_ptr<InetAddress> from(const std::string& host, PortNumber port);
  static std::unique_ptr<InetAddress> from(sockaddr* addr);

 protected:
  InetProtocolVersion ipv_;
  std::string readable_;
  SockLen addr_len_ = 0;
};

class InetAddressIPv4 : public InetAddress {
 public:
  explicit InetAddressIPv4(PortNumber port);

  InetAddressIPv4(IPv4Address ip, PortNumber port);

  InetAddressIPv4(const std::string& str, PortNumber port);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD const sockaddr* handle_ptr() const override;

  ZNET_NODISCARD PortNumber port() const override;

  ZNET_NODISCARD std::unique_ptr<InetAddress> WithPort(PortNumber port) const override;

  ZNET_NODISCARD std::string host_key() const override;

  /** @brief The host bytes in network order, as they sit on the wire. */
  ZNET_NODISCARD IPv4Address address() const;

 private:
  // holds a sockaddr_in; inet_addr.cc asserts the size and alignment fit
  alignas(8) unsigned char storage_[16] = {};
  bool is_valid_;
};

class InetAddressIPv6 : public InetAddress {
 public:
  explicit InetAddressIPv6(PortNumber port);
  InetAddressIPv6(IPv6Address ip, PortNumber port);
  InetAddressIPv6(const std::string& str, PortNumber port);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD const sockaddr* handle_ptr() const override;

  ZNET_NODISCARD PortNumber port() const override;

  ZNET_NODISCARD std::unique_ptr<InetAddress> WithPort(PortNumber port) const override;

  ZNET_NODISCARD std::string host_key() const override;

  /** @brief The host bytes in network order, as they sit on the wire. */
  ZNET_NODISCARD IPv6Address address() const;

 private:
  // holds a sockaddr_in6; inet_addr.cc asserts the size and alignment fit
  alignas(8) unsigned char storage_[32] = {};
  bool is_valid_;
};

#if ZNET_HAS_AF_UNIX
/**
 * @brief A Unix domain socket path.
 *
 * Spelled `unix:/path` wherever a host string is accepted, e.g.
 * `ServerConfig{"unix:/run/app.sock", 0, ...}` with ConnectionType::TCP.
 * Ports do not apply and read back as 0. An empty path is the unnamed
 * address a connected client shows up as.
 */
class InetAddressUnix : public InetAddress {
 public:
  explicit InetAddressUnix(const std::string& path);

  ZNET_NODISCARD bool is_valid() const override { return is_valid_; }

  ZNET_NODISCARD const sockaddr* handle_ptr() const override;

  /** @brief Paths have no ports; always 0. */
  ZNET_NODISCARD PortNumber port() const override { return 0; }

  /** @brief Paths have no ports; returns a copy of this address. */
  ZNET_NODISCARD std::unique_ptr<InetAddress> WithPort(PortNumber port) const override;

  ZNET_NODISCARD const char* path() const;

 private:
  // holds a sockaddr_un; inet_addr.cc asserts the size and alignment fit
  alignas(8) unsigned char storage_[128] = {};
  bool is_valid_ = false;
};
#endif  // ZNET_HAS_AF_UNIX

/**
 * @brief One allow/deny rule: an address block in CIDR notation.
 *
 * Parses "10.0.0.0/8", a bare host like "192.168.1.5", "2001:db8::/32" or
 * "::1". IPv4 rules also match IPv4-mapped IPv6 sources, which is what a v4
 * client looks like to a dual-stack listener; native IPv6 sources match only
 * IPv6 rules.
 */
class CIDRBlock {
 public:
  CIDRBlock() = default;

  static CIDRBlock Parse(const std::string& text);

  ZNET_NODISCARD bool is_valid() const { return is_valid_; }

  /** @brief What Parse() was given, for logs. */
  ZNET_NODISCARD const std::string& text() const { return text_; }

  ZNET_NODISCARD bool Matches(const InetAddress& address) const;

 private:
  std::string text_;
  unsigned char bytes_[16] = {};
  uint8_t prefix_ = 0;
  bool is_ipv6_ = false;
  bool is_valid_ = false;
};

}  // namespace znet

#endif  // ZNET_INET_ADDR_H_
