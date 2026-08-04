//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/backends/backend.h"
#include "znet/backends/tcp.h"
#include "znet/backends/zdt.h"
#include "znet/logger.h"

namespace znet {
namespace backends {

namespace {

// ZDT runs over UDP; a Unix path only speaks the stream backend.
bool RefusedByZDT(const std::shared_ptr<InetAddress>& address) {
  if (address && address->ipv() == InetProtocolVersion::Unix) {
    ZNET_LOG_ERROR(
        "ZDT does not support Unix domain sockets; use ConnectionType::TCP "
        "for {}.",
        address->readable());
    return true;
  }
  return false;
}

}  // namespace

std::unique_ptr<ClientBackend> CreateClientFromType(
    ConnectionType type, std::shared_ptr<InetAddress> server_address,
    const SessionOptions& options) {
  if (type == ConnectionType::TCP) {
    return std::make_unique<TCPClientBackend>(server_address, options);
  }
  if (type == ConnectionType::ZDT) {
    if (RefusedByZDT(server_address)) {
      return nullptr;
    }
    return std::make_unique<ZDTClientBackend>(server_address, options);
  }
  return nullptr;
}

std::unique_ptr<ServerBackend> CreateServerFromType(
    ConnectionType type, std::shared_ptr<InetAddress> bind_address,
    const SessionOptions& child_options, const ServerOptions& server_options) {
  if (type == ConnectionType::TCP) {
    return std::make_unique<TCPServerBackend>(bind_address, child_options,
                                              server_options);
  }
  if (type == ConnectionType::ZDT) {
    if (RefusedByZDT(bind_address)) {
      return nullptr;
    }
    return std::make_unique<ZDTServerBackend>(bind_address, child_options,
                                              server_options);
  }
  return nullptr;
}

}
}