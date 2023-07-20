//
//    Copyright 2023 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring> // For memset
#include "znet/znet.h"

int main() {
  int client_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (client_socket_ < 0) {
    std::cerr << "Error creating socket." << std::endl;
    return 1;
  }

  znet::Ref<znet::InetAddress> address = znet::InetAddress::from("127.0.0.1", 25000);
  if (connect(client_socket_, address->handle_ptr(), address->addr_size()) < 0) {
    std::cerr << "Error connecting to server." << std::endl;
    return 1;
  }
  int option = 1;
  setsockopt(client_socket_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(option));

  // Connected to the server
  LOG_INFO("Connected to the server.");

  uint64_t i = UINT32_MAX;
  while(i > 0) {
    i--;
  }
  // Send data to the server
  znet::Buffer buffer;
  buffer.WriteInt(znet::PacketId(1));
  buffer.WriteString("Hello world!");
  if (send(client_socket_, buffer.data(), buffer.size(), 0) < 0) {
    std::cerr << "Error sending data to the server." << std::endl;
    return 1;
  }
  LOG_INFO("Data sent.");

  // Handle the client connection
  char buffer_[MAX_BUFFER_SIZE];
  ssize_t data_size_ = recv(client_socket_, buffer_, sizeof(buffer_), 0);
  LOG_INFO("{}", data_size_);
  auto buf = znet::CreateRef<znet::Buffer>(buffer_, data_size_);
  LOG_INFO("{}", buf->ReadString());

  i = UINT32_MAX;
  while(i > 0) {
    i--;
  }

  LOG_INFO("Disconnecting.");
  // Close the client socket
  close(client_socket_);

  return 0;
}
