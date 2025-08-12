cmake_minimum_required(VERSION 3.16.3)
project(znet-parent)

add_subdirectory(vendor/fmt)
add_subdirectory(vendor/zstd/build/cmake)
add_subdirectory(znet)

# rendezvous server
add_subdirectory(rendezvous-server)

# examples
add_subdirectory(examples/basic/server)
add_subdirectory(examples/basic/client)
add_subdirectory(examples/p2p)
add_subdirectory(examples/multiversion/server)
add_subdirectory(examples/multiversion/client-v2)
add_subdirectory(examples/multiversion/client-v1)

# tests
enable_testing()
add_subdirectory(tests)