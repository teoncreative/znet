cmake_minimum_required(VERSION 3.25)
project(znet-parent)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(znet)

# examples
add_subdirectory(examples/demo-server)
add_subdirectory(examples/demo-client)
add_subdirectory(examples/buffer-test)
