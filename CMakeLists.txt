cmake_minimum_required(VERSION 3.29)

project(znet-parent
    DESCRIPTION "Build for the znet library, its examples and its tests"
    LANGUAGES CXX
)

# The whole tree follows one standard so that -DZNET_CXX_STANDARD=14 really
# does prove a C++14 consumer can use the public headers, not merely that the
# library compiled at 14 and a C++20 program linked it. znet/CMakeLists.txt
# declares the cache entry; setting it here first means every subproject sees
# the same value.
set(ZNET_CXX_STANDARD "20" CACHE STRING "C++ standard for znet and every subproject (14, 17, 20 or 23)")
set_property(CACHE ZNET_CXX_STANDARD PROPERTY STRINGS 14 17 20 23)

# Applies the selected standard to a target. Subprojects call this instead of
# hardcoding CXX_STANDARD, so a single flag rebuilds the tree at any level.
function(znet_apply_cxx_standard target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${ZNET_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

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

# benchmarks (off by default; they are slow and can download comparison libraries)
option(ZNET_BUILD_BENCHMARKS "Build the benchmark suite" OFF)
if(ZNET_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()