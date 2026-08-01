cmake_minimum_required(VERSION 3.29)

# znet-delta: send only what changed since a baseline the peer acknowledged.
#
# No third-party dependency, but it does build on znet-bitpack: a per-field
# "changed" flag is only worth having when a flag costs one bit rather than a
# byte, so the two are not really separable.

option(ZNET_EXT_DELTA "Build the delta compression extension" ON)
if(NOT ZNET_EXT_DELTA)
    return()
endif()

if(NOT TARGET znet-bitpack)
    message(STATUS "znet-delta: needs znet-bitpack, which is off; skipping extension")
    return()
endif()

add_library(znet-delta INTERFACE)
add_library(znet::delta ALIAS znet-delta)

target_include_directories(znet-delta INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-delta INTERFACE znet znet-bitpack)
target_compile_features(znet-delta INTERFACE cxx_std_14)

message(STATUS "znet-delta: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-delta-tests tests/delta_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-delta-tests)
    endif()
    target_link_libraries(znet-ext-delta-tests PRIVATE gtest_main znet-delta)

    target_compile_options(znet-ext-delta-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-delta-tests COMMAND znet-ext-delta-tests)
else()
    message(STATUS "znet-delta: googletest not available, extension tests skipped")
endif()
