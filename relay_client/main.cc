//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <utility>

#include <deque>
#include "cxxopts.h"
#include "znet/p2p/relay.h"
#include "znet/p2p/locator.h"
#include "znet/p2p/dialer.h"
#include "znet/znet.h"
#include "znet/base/inet_addr.h"

std::unique_ptr<znet::p2p::PeerLocator> locator_;

bool OnReady(znet::p2p::PeerLocatorReadyEvent& event) {
  ZNET_LOG_INFO("Received peer name from relay: {}", event.peer_name());
  std::string peer_name;
  ZNET_LOG_INFO("Enter peer name:");
  std::cin >> peer_name;
  locator_->AskPeer(peer_name);
  return false;
}

bool OnPunchRequest(znet::p2p::StartPunchRequestEvent& event) {
  locator_->Close();
  ZNET_LOG_INFO("Received punch request to {} at {}", event.target_peer(), event.target_endpoint()->readable());
  auto result = znet::p2p::Dialer::Punch(
      event.bind_endpoint(),
      event.target_endpoint()
  );
  ZNET_LOG_INFO("Result: {}", GetResultString(result));
  return false;
}

void OnEvent(znet::Event& event) {
  znet::EventDispatcher dispatcher{event};
  dispatcher.Dispatch<znet::p2p::PeerLocatorReadyEvent>(
      ZNET_BIND_GLOBAL_FN(OnReady));
  dispatcher.Dispatch<znet::p2p::StartPunchRequestEvent>(
      ZNET_BIND_GLOBAL_FN(OnPunchRequest));
}

int main(int argc, char* argv[]) {
  cxxopts::Options opts(
      "relay-client",
      "relay-client is a test utility to test peer to peer connections");
  opts.add_options()
      ("t,target", "Address of the relay server", cxxopts::value<std::string>())
      ("p,port", "Port of the relay server",cxxopts::value<uint16_t>()->default_value("5001"))
          ("h,help", "Print usage");

  auto result = opts.parse(argc, argv);
  if (result["help"].as<bool>()) {
    std::cout << opts.help() << "\n";
    return 0;
  }

  uint16_t port = result["port"].as<uint16_t>();
  std::string host = result["target"].as<std::string>();

  ZNET_LOG_INFO("Connecting to relay on {}:{}...", host, port);
  znet::p2p::PeerLocatorConfig config{host, port};
  locator_ = std::make_unique<znet::p2p::PeerLocator>(config);
  locator_->SetEventCallback(ZNET_BIND_GLOBAL_FN(OnEvent));

  if (locator_->Start() != znet::Result::Success) {
    return 1;  // Failed to bind
  }
  locator_->Wait();
  while (true) {

  }
  return 0;
}