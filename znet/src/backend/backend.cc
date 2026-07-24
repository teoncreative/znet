//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Created by Metehan Gezer on 13/08/2025.
//

#include "znet/backends/backend.h"
#include "znet/backends/tcp.h"
#include "znet/backends/zdt.h"

namespace znet {
namespace backends {

std::unique_ptr<ClientBackend> CreateClientFromType(
    ConnectionType type, std::shared_ptr<InetAddress> server_address,
    const SessionOptions& options) {
  if (type == ConnectionType::TCP) {
    return std::make_unique<TCPClientBackend>(server_address, options);
  }
  if (type == ConnectionType::ZDT) {
    return std::make_unique<ZDTClientBackend>(server_address, options);
  }
  return nullptr;
}

std::unique_ptr<ServerBackend> CreateServerFromType(
    ConnectionType type, std::shared_ptr<InetAddress> bind_address,
    const SessionOptions& child_options) {
  if (type == ConnectionType::TCP) {
    return std::make_unique<TCPServerBackend>(bind_address, child_options);
  }
  if (type == ConnectionType::ZDT) {
    return std::make_unique<ZDTServerBackend>(bind_address, child_options);
  }
  return nullptr;
}


}
}