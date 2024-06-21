cmake_minimum_required(VERSION 3.16.3)
project(znet-parent)

set(CMAKE_CXX_STANDARD 20)

message("Hello world!")

add_subdirectory(vendor/fmt)
add_subdirectory(znet)

# examples
add_subdirectory(examples/example-server)
add_subdirectory(examples/example-client)

# tests
enable_testing()
add_subdirectory(tests)