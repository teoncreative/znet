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
#include "znet/detail/sys_net.h"
#include "znet/logger.h"
#include "znet/init.h"

// Enumerating the host's own interfaces, which sys_net.h does not cover.
#ifdef ZNET_TARGET_WIN
#include <iphlpapi.h>
#elif defined(ZNET_TARGET_POSIX)
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <regex>
#include <type_traits>

namespace znet {

// The public header describes each address by size alone, so this is the one
// place that gets to see whether the description still matches the platform.
static_assert(std::is_same<SockLen, socklen_t>::value,
              "znet::SockLen must be the platform's socklen_t");
static_assert(sizeof(SocketHandle) == sizeof(decltype(socket(0, 0, 0))),
              "znet::SocketHandle must be the platform's socket handle");
static_assert(std::is_signed<SocketHandle>::value ==
                  std::is_signed<decltype(socket(0, 0, 0))>::value,
              "znet::SocketHandle disagrees with the platform on signedness");
static_assert(sizeof(IPv4Address) == sizeof(in_addr), "in_addr is not 4 bytes");
static_assert(sizeof(IPv6Address) == sizeof(in6_addr),
              "in6_addr is not 16 bytes");

namespace {

// Reads back the sockaddr a constructor placement-new'd into an address's
// storage. Every constructor does so before anything else, including on its
// failure paths, so this is never handed uninitialized bytes.
template <typename T, size_t N>
T* AddrAs(unsigned char (&storage)[N]) {
  static_assert(sizeof(T) <= N, "sockaddr does not fit in the storage");
  static_assert(alignof(T) <= 8, "sockaddr wants more than 8-byte alignment");
  return reinterpret_cast<T*>(storage);
}

template <typename T, size_t N>
const T* AddrAs(const unsigned char (&storage)[N]) {
  static_assert(sizeof(T) <= N, "sockaddr does not fit in the storage");
  static_assert(alignof(T) <= 8, "sockaddr wants more than 8-byte alignment");
  return reinterpret_cast<const T*>(storage);
}

in_addr ToInAddr(IPv4Address ip) {
  in_addr out{};
  std::memcpy(&out, ip.bytes, sizeof(ip.bytes));
  return out;
}

in6_addr ToIn6Addr(IPv6Address ip) {
  in6_addr out{};
  std::memcpy(&out, ip.bytes, sizeof(ip.bytes));
  return out;
}

IPv4Address FromInAddr(const in_addr& in) {
  IPv4Address out{};
  std::memcpy(out.bytes, &in, sizeof(out.bytes));
  return out;
}

IPv6Address FromIn6Addr(const in6_addr& in) {
  IPv6Address out{};
  std::memcpy(out.bytes, &in, sizeof(out.bytes));
  return out;
}

}  // namespace

IPv4Address ParseIPv4(const std::string& ip_str) {
  IPv4Address addr{};
  inet_pton(AF_INET, ip_str.c_str(), addr.bytes);
  return addr;
}

IPv6Address ParseIPv6(const std::string& ip_str) {
  IPv6Address addr{};
  inet_pton(AF_INET6, ip_str.c_str(), addr.bytes);
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


namespace {

const char* LoopbackAddress(InetProtocolVersion version) {
  return version == InetProtocolVersion::IPv6 ? "::1" : "127.0.0.1";
}

// Unreachable off-link, so only noise as candidates.
bool IsLinkLocal(InetProtocolVersion version, const std::string& ip) {
  if (version == InetProtocolVersion::IPv6) {
    return ip.rfind("fe80:", 0) == 0 || ip.rfind("FE80:", 0) == 0;
  }
  return ip.rfind("169.254.", 0) == 0;
}

bool IsLoopback(InetProtocolVersion version, const std::string& ip) {
  if (version == InetProtocolVersion::IPv6) return ip == "::1";
  return ip.rfind("127.", 0) == 0;
}

// Container and hypervisor bridges: nothing off-host routes to them. VPN
// interfaces are kept, since reaching a peer over one is a real case.
bool IsVirtualBridge(const char* name) {
  if (!name) return false;
  static const char* kPrefixes[] = {"docker", "br-",    "veth",
                                    "virbr",  "vmnet",  "vboxnet"};
  for (const char* prefix : kPrefixes) {
    if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
  }
  return false;
}

// Candidates ride in every registration and punch packet; a dev box can carry
// a dozen addresses.
constexpr size_t kMaxLocalAddresses = 8;

}  // namespace

std::vector<std::string> GetLocalAddresses(InetProtocolVersion version) {
  const int family =
      version == InetProtocolVersion::IPv6 ? AF_INET6 : AF_INET;
  (void)family;  // unused where neither branch below compiles, e.g. web
  std::vector<std::string> addresses;
  bool has_loopback = false;

#ifdef ZNET_TARGET_WIN
  ULONG size = 15 * 1024;  // what the API docs suggest starting from
  std::vector<char> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                      GAA_FLAG_SKIP_DNS_SERVER;

  ULONG result = GetAdaptersAddresses(static_cast<ULONG>(family), flags, nullptr,
                                      adapters, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(static_cast<ULONG>(family), flags, nullptr,
                                  adapters, &size);
  }

  if (result == NO_ERROR) {
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp) continue;
      for (auto* unicast = adapter->FirstUnicastAddress; unicast;
           unicast = unicast->Next) {
        char text[INET6_ADDRSTRLEN] = {};
        if (getnameinfo(unicast->Address.lpSockaddr,
                        unicast->Address.iSockaddrLength, text, sizeof(text),
                        nullptr, 0, NI_NUMERICHOST) != 0) {
          continue;
        }
        std::string ip(text);
        // Windows appends a scope id to IPv6, which is meaningless elsewhere.
        const size_t scope = ip.find('%');
        if (scope != std::string::npos) ip.erase(scope);

        if (IsLinkLocal(version, ip)) continue;
        if (IsLoopback(version, ip)) {
          has_loopback = true;
          continue;
        }
        addresses.push_back(ip);
      }
    }
  } else {
    ZNET_LOG_DEBUG("GetAdaptersAddresses failed: {}", result);
  }
#elif defined(ZNET_TARGET_POSIX)
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) == 0) {
    for (auto* it = interfaces; it; it = it->ifa_next) {
      if (!it->ifa_addr) continue;
      if (it->ifa_addr->sa_family != family) continue;
      if (!(it->ifa_flags & IFF_UP)) continue;
      if (IsVirtualBridge(it->ifa_name)) continue;

      const socklen_t length = family == AF_INET6 ? sizeof(sockaddr_in6)
                                                  : sizeof(sockaddr_in);
      char text[INET6_ADDRSTRLEN] = {};
      if (getnameinfo(it->ifa_addr, length, text, sizeof(text), nullptr, 0,
                      NI_NUMERICHOST) != 0) {
        continue;
      }
      std::string ip(text);
      const size_t scope = ip.find('%');
      if (scope != std::string::npos) ip.erase(scope);

      if (IsLinkLocal(version, ip)) continue;
      if (IsLoopback(version, ip)) {
        has_loopback = true;
        continue;
      }
      addresses.push_back(ip);
    }
    freeifaddrs(interfaces);
  } else {
    ZNET_LOG_DEBUG("getifaddrs failed, falling back to loopback");
  }
#endif

  // Last: only connects a host to itself, but that case is real.
  if (addresses.size() > kMaxLocalAddresses) {
    addresses.resize(kMaxLocalAddresses);
  }
  if (has_loopback || addresses.empty()) {
    addresses.emplace_back(LoopbackAddress(version));
  }
  return addresses;
}

std::string GetLoopbackAddress(InetProtocolVersion version) {
  if (version == InetProtocolVersion::Unix) {
    ZNET_LOG_ERROR("Invalid InetProtocolVersion: {}",
                   static_cast<int>(version));
    return LoopbackAddress(InetProtocolVersion::IPv4);
  }
  return LoopbackAddress(version);
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
    return std::make_unique<InetAddressIPv4>(FromInAddr(addr->sin_addr),
                                             ntohs(addr->sin_port));
  } else if (sock_addr->sa_family == AF_INET6) {
    auto* addr = reinterpret_cast<sockaddr_in6*>(sock_addr);
    return std::make_unique<InetAddressIPv6>(FromIn6Addr(addr->sin6_addr),
                                             ntohs(addr->sin6_port));
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
    : InetAddressIPv4(IPv4Address{}, port) {}

InetAddressIPv4::InetAddressIPv4(IPv4Address ip, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv4, "") {
  auto* addr = new (storage_) sockaddr_in{};
  addr_len_ = sizeof(sockaddr_in);
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr = ToInAddr(ip);
#ifdef ZNET_TARGET_APPLE
  addr->sin_len = sizeof(sockaddr_in);
#endif

  char src[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr->sin_addr, src, INET_ADDRSTRLEN);
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr->sin_port));
  is_valid_ = true;
}

InetAddressIPv4::InetAddressIPv4(const std::string& str, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv4, "") {
  auto* addr = new (storage_) sockaddr_in{};
  addr_len_ = sizeof(sockaddr_in);
  if (!IsIPv4(str)) {
    is_valid_ = false;
    readable_ = "Invalid Address";
    return;
  }

  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr = ToInAddr(ParseIPv4(str));
#ifdef ZNET_TARGET_APPLE
  addr->sin_len = sizeof(sockaddr_in);
#endif
  char src[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr->sin_addr, src, INET_ADDRSTRLEN);
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr->sin_port));
  is_valid_ = true;
}

const sockaddr* InetAddressIPv4::handle_ptr() const {
  return reinterpret_cast<const sockaddr*>(AddrAs<sockaddr_in>(storage_));
}

PortNumber InetAddressIPv4::port() const {
  return ntohs(AddrAs<sockaddr_in>(storage_)->sin_port);
}

std::unique_ptr<InetAddress> InetAddressIPv4::WithPort(PortNumber port) const {
  return std::make_unique<InetAddressIPv4>(address(), port);
}

std::string InetAddressIPv4::host_key() const {
  const IPv4Address ip = address();
  return std::string(reinterpret_cast<const char*>(ip.bytes), sizeof(ip.bytes));
}

IPv4Address InetAddressIPv4::address() const {
  return FromInAddr(AddrAs<sockaddr_in>(storage_)->sin_addr);
}

InetAddressIPv6::InetAddressIPv6(PortNumber port)
    : InetAddressIPv6(IPv6Address{}, port) {}

InetAddressIPv6::InetAddressIPv6(IPv6Address ip, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv6, "") {
  auto* addr = new (storage_) sockaddr_in6{};
  addr_len_ = sizeof(sockaddr_in6);
  addr->sin6_family = AF_INET6;
  addr->sin6_flowinfo = 0;
  addr->sin6_port = htons(port);
  addr->sin6_addr = ToIn6Addr(ip);
#if !defined(ZNET_TARGET_WIN) && !defined(ZNET_TARGET_WEB) && !defined(ZNET_TARGET_LINUX)
  addr->sin6_len = sizeof(sockaddr_in6);
#endif
  char src[INET6_ADDRSTRLEN];
  inet_ntop(AF_INET6, &addr->sin6_addr, src, sizeof(src));
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr->sin6_port));
  is_valid_ = true;
}

InetAddressIPv6::InetAddressIPv6(const std::string& str, PortNumber port)
    : InetAddress(InetProtocolVersion::IPv6, "") {
  auto* addr = new (storage_) sockaddr_in6{};
  addr_len_ = sizeof(sockaddr_in6);
  if (!IsIPv6(str)) {
    is_valid_ = false;
    readable_ = "Invalid Address";
    return;
  }
#if !defined(ZNET_TARGET_WIN) && !defined(ZNET_TARGET_WEB) && !defined(ZNET_TARGET_LINUX)
  addr->sin6_len = sizeof(sockaddr_in6);
#endif
  addr->sin6_family = AF_INET6;
  addr->sin6_flowinfo = 0;
  addr->sin6_port = htons(port);
  addr->sin6_addr = ToIn6Addr(ParseIPv6(str));
  char src[INET6_ADDRSTRLEN];
  inet_ntop(AF_INET6, &addr->sin6_addr, src, sizeof(src));
  readable_ = std::string(src) + ":" + std::to_string(ntohs(addr->sin6_port));
  is_valid_ = true;
}

const sockaddr* InetAddressIPv6::handle_ptr() const {
  return reinterpret_cast<const sockaddr*>(AddrAs<sockaddr_in6>(storage_));
}

PortNumber InetAddressIPv6::port() const {
  return ntohs(AddrAs<sockaddr_in6>(storage_)->sin6_port);
}

std::unique_ptr<InetAddress> InetAddressIPv6::WithPort(PortNumber port) const {
  return std::make_unique<InetAddressIPv6>(address(), port);
}

IPv6Address InetAddressIPv6::address() const {
  return FromIn6Addr(AddrAs<sockaddr_in6>(storage_)->sin6_addr);
}

#if ZNET_HAS_AF_UNIX
InetAddressUnix::InetAddressUnix(const std::string& path)
    : InetAddress(InetProtocolVersion::Unix, "") {
  auto* addr = new (storage_) sockaddr_un{};
  if (path.size() >= sizeof(addr->sun_path)) {
    ZNET_LOG_WARN("Unix socket path is too long ({} bytes, limit {}): {}",
                  path.size(), sizeof(addr->sun_path) - 1, path);
    readable_ = "Invalid Address";
    return;
  }
  addr->sun_family = AF_UNIX;
  std::memcpy(addr->sun_path, path.data(), path.size());
  addr->sun_path[path.size()] = '\0';
  addr_len_ = static_cast<SockLen>(offsetof(sockaddr_un, sun_path) +
                                   path.size() + 1);
#ifdef ZNET_TARGET_APPLE
  addr->sun_len = static_cast<unsigned char>(addr_len_);
#endif
  readable_ = "unix:" + path;
  is_valid_ = true;
}

const sockaddr* InetAddressUnix::handle_ptr() const {
  return reinterpret_cast<const sockaddr*>(AddrAs<sockaddr_un>(storage_));
}

const char* InetAddressUnix::path() const {
  return AddrAs<sockaddr_un>(storage_)->sun_path;
}

std::unique_ptr<InetAddress> InetAddressUnix::WithPort(PortNumber port) const {
  (void)port;  // paths have no ports
  return std::make_unique<InetAddressUnix>(std::string(path()));
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
  const IPv6Address ip = address();
  if (IsV4MappedV6(ip.bytes)) {
    return std::string(reinterpret_cast<const char*>(ip.bytes + 12), 4);
  }
  return std::string(reinterpret_cast<const char*>(ip.bytes), 16);
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