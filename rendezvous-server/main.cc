//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "znet/p2p/rendezvous_server.h"

#include "cxxopts.h"

int main(int argc, char* argv[]) {
  cxxopts::Options opts(
      "relay-server",
      "relay-server is a utility for znet that exchanges peer endpoints");
  opts.add_options()
      ("p,port", "Port to listen on",
                     cxxopts::value<uint16_t>()->default_value("5001"))
          ("t,target", "Host to listen on",
           cxxopts::value<std::string>()->default_value("0.0.0.0"))
              ("c,conn", "Punch connection type: tcp or zdt",
               cxxopts::value<std::string>()->default_value("zdt"))
              ("h,help", "Print usage");

  auto result = opts.parse(argc, argv);
  if (result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  std::string conn = result["conn"].as<std::string>();
  znet::p2p::RendezvousServer::Config config;
  config.bind_port = result["port"].as<uint16_t>();
  config.bind_address = result["target"].as<std::string>();
  config.punch_connection_type = conn == "tcp" ? znet::ConnectionType::TCP
                                               : znet::ConnectionType::ZDT;
  ZNET_LOG_INFO("Starting relay on {}:{}... (punch type: {})", config.bind_address,
                config.bind_port, conn == "tcp" ? "tcp" : "zdt");

  znet::p2p::RendezvousServer relay{config};
  if (relay.Start() != znet::Result::Success) {
    return 1;
  }
  relay.Wait();
  return 0;
}
