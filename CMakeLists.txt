cmake_minimum_required(VERSION 3.29)

project(znet-parent
    DESCRIPTION "Build for the znet library, its examples and its tests"
    LANGUAGES CXX
)

add_subdirectory(vendor/zstd/build/cmake)
add_subdirectory(znet)

# rendezvous server
add_subdirectory(rendezvous-server)

# examples
add_subdirectory(examples/basic/server)
add_subdirectory(examples/basic/client)
add_subdirectory(examples/zdt/server)
add_subdirectory(examples/zdt/client)
add_subdirectory(examples/p2p)
add_subdirectory(examples/multiversion/server)
add_subdirectory(examples/multiversion/client-v2)
add_subdirectory(examples/multiversion/client-v1)

# tests
enable_testing()
add_subdirectory(tests)