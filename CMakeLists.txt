cmake_minimum_required(VERSION 3.16.3)
project(znet-parent)

add_subdirectory(vendor/fmt)
add_subdirectory(znet)
add_subdirectory(relay_server)

# examples
add_subdirectory(examples/basic/server)
add_subdirectory(examples/basic/client)
add_subdirectory(examples/relay_client)

# tests
enable_testing()
add_subdirectory(tests)