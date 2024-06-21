cmake_minimum_required(VERSION 3.16.3)
project(znet-parent)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(vendor/fmt)
set(fmt_DIR "${CMAKE_SOURCE_DIR}/vendor/fmt")
message("fmt_DIR: ${fmt_DIR}")
add_subdirectory(znet)

# examples
add_subdirectory(examples/example-server)
add_subdirectory(examples/example-client)

# tests
enable_testing()
add_subdirectory(tests)