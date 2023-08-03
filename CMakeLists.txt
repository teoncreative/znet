cmake_minimum_required(VERSION 3.25)
project(znet-parent)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(vendor/fmt)
add_subdirectory(znet)
add_subdirectory(examples/demo-server)
add_subdirectory(examples/demo-client)
add_subdirectory(examples/buffer-test)
add_subdirectory(examples/chat)

