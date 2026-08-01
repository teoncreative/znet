cmake_minimum_required(VERSION 3.29)

# znet-box2d: planar rigid body state on the wire.

option(ZNET_EXT_BOX2D "Build the Box2D extension" ON)
if(NOT ZNET_EXT_BOX2D)
    return()
endif()

if(NOT TARGET znet-bitpack)
    message(STATUS "znet-box2d: needs znet-bitpack, which is off; skipping extension")
    return()
endif()

if(NOT TARGET box2d)
    find_package(box2d CONFIG QUIET)
endif()

if(NOT TARGET box2d)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-box2d: Box2D not installed, fetching it")
        include(FetchContent)
        set(BOX2D_SAMPLES OFF CACHE INTERNAL "")
        set(BOX2D_UNIT_TESTS OFF CACHE INTERNAL "")
        set(BOX2D_BENCHMARKS OFF CACHE INTERNAL "")
        set(BOX2D_DOCS OFF CACHE INTERNAL "")
        set(BOX2D_PROFILE OFF CACHE INTERNAL "")
        set(BOX2D_VALIDATE OFF CACHE INTERNAL "")
        FetchContent_Declare(
            box2d
            URL https://github.com/erincatto/box2d/archive/refs/tags/v3.1.0.tar.gz
            SYSTEM
            URL_HASH SHA256=7fac19801485efb31ee3745b2284d9d4601f9e8138a3383a7b0df6d788ea5785
        )
        FetchContent_MakeAvailable(box2d)
        znet_ext_relax_dependency_warnings(box2d)
        znet_ext_match_msvc_runtime(box2d)
    else()
        message(STATUS "znet-box2d: Box2D not found and fetching is off, skipping extension")
        return()
    endif()
endif()

add_library(znet-box2d INTERFACE)
add_library(znet::box2d ALIAS znet-box2d)

target_include_directories(znet-box2d INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-box2d INTERFACE znet znet-quantize znet-bitpack box2d)
target_compile_features(znet-box2d INTERFACE cxx_std_14)

message(STATUS "znet-box2d: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-box2d-tests tests/box2d_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-box2d-tests)
    endif()
    target_link_libraries(znet-ext-box2d-tests PRIVATE gtest_main znet-box2d)

    target_compile_options(znet-ext-box2d-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-box2d-tests COMMAND znet-ext-box2d-tests)
else()
    message(STATUS "znet-box2d: googletest not available, extension tests skipped")
endif()
