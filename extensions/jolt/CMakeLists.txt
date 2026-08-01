cmake_minimum_required(VERSION 3.29)

# znet-jolt: Jolt rigid body state on the wire.

option(ZNET_EXT_JOLT "Build the Jolt extension" ON)
if(NOT ZNET_EXT_JOLT)
    return()
endif()

if(NOT TARGET znet-bitpack)
    message(STATUS "znet-jolt: needs znet-bitpack, which is off; skipping extension")
    return()
endif()

# Jolt is a C++17 library and its headers use if constexpr throughout.
if(ZNET_CXX_STANDARD LESS 17)
    message(STATUS "znet-jolt: Jolt requires C++17 and the tree is at C++${ZNET_CXX_STANDARD}; skipping extension")
    return()
endif()

if(NOT TARGET Jolt)
    find_package(Jolt CONFIG QUIET)
endif()

if(NOT TARGET Jolt)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-jolt: Jolt not installed, fetching it (18 MB)")
        include(FetchContent)
        set(TARGET_UNIT_TESTS OFF CACHE INTERNAL "")
        set(TARGET_HELLO_WORLD OFF CACHE INTERNAL "")
        set(TARGET_PERFORMANCE_TEST OFF CACHE INTERNAL "")
        set(TARGET_SAMPLES OFF CACHE INTERNAL "")
        set(TARGET_VIEWER OFF CACHE INTERNAL "")
        set(ENABLE_ALL_WARNINGS OFF CACHE INTERNAL "")
        FetchContent_Declare(
            JoltPhysics
            URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.2.0.tar.gz
            SYSTEM
            URL_HASH SHA256=f478afe3050c885e21403748e10ab18e3e8df8b0982c540e75f1e078ef8b2c88
            SOURCE_SUBDIR Build
        )
        FetchContent_MakeAvailable(JoltPhysics)
    else()
        message(STATUS "znet-jolt: Jolt not found and fetching is off, skipping extension")
        return()
    endif()
endif()

if(NOT TARGET Jolt)
    message(STATUS "znet-jolt: Jolt did not provide the Jolt target, skipping extension")
    return()
endif()

add_library(znet-jolt INTERFACE)
add_library(znet::jolt ALIAS znet-jolt)

target_include_directories(znet-jolt INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-jolt INTERFACE znet znet-quantize znet-bitpack Jolt)
target_compile_features(znet-jolt INTERFACE cxx_std_17)

message(STATUS "znet-jolt: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-jolt-tests tests/jolt_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-jolt-tests)
    endif()
    target_link_libraries(znet-ext-jolt-tests PRIVATE gtest_main znet-jolt)

    target_compile_options(znet-ext-jolt-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-jolt-tests COMMAND znet-ext-jolt-tests)
else()
    message(STATUS "znet-jolt: googletest not available, extension tests skipped")
endif()
