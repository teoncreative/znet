cmake_minimum_required(VERSION 3.29)

# znet-bitpack: sub-byte fields in a Buffer.
#
# The only extension with no third-party dependency, so there is nothing to
# find and nothing to skip for.

option(ZNET_EXT_BITPACK "Build the bit packing extension" ON)
if(NOT ZNET_EXT_BITPACK)
    return()
endif()

if(NOT TARGET znet-quantize)
    message(STATUS "znet-bitpack: needs znet-quantize, which is off; skipping extension")
    return()
endif()

add_library(znet-bitpack INTERFACE)
add_library(znet::bitpack ALIAS znet-bitpack)

target_include_directories(znet-bitpack INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-bitpack INTERFACE znet znet-quantize)
target_compile_features(znet-bitpack INTERFACE cxx_std_14)

message(STATUS "znet-bitpack: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-bitpack-tests tests/bitpack_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-bitpack-tests)
    endif()
    target_link_libraries(znet-ext-bitpack-tests PRIVATE gtest_main znet-bitpack)

    target_compile_options(znet-ext-bitpack-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-bitpack-tests COMMAND znet-ext-bitpack-tests)
else()
    message(STATUS "znet-bitpack: googletest not available, extension tests skipped")
endif()
