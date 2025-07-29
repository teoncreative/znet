cmake_minimum_required(VERSION 3.16.3)
project(znet-parent)

add_subdirectory(vendor/fmt)
add_subdirectory(znet)

# examples
add_subdirectory(examples/basic/server)
add_subdirectory(examples/basic/client)

# tests
enable_testing()
add_subdirectory(tests)