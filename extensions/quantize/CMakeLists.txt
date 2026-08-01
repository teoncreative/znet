cmake_minimum_required(VERSION 3.29)

# znet-quantize: the quantisation arithmetic, on plain scalars.
#
# Depends on nothing, not even a Buffer. Everything that compresses a value for
# the wire routes through here rather than carrying its own copy, because two
# copies that disagree by one rounding produce a stream that mostly works.

option(ZNET_EXT_QUANTIZE "Build the quantisation core" ON)
if(NOT ZNET_EXT_QUANTIZE)
    return()
endif()

add_library(znet-quantize INTERFACE)
add_library(znet::quantize ALIAS znet-quantize)

target_include_directories(znet-quantize INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-quantize INTERFACE znet)
target_compile_features(znet-quantize INTERFACE cxx_std_14)

message(STATUS "znet-quantize: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-quantize-tests tests/quantize_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-quantize-tests)
    endif()
    target_link_libraries(znet-ext-quantize-tests PRIVATE gtest_main znet-quantize)

    target_compile_options(znet-ext-quantize-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-quantize-tests COMMAND znet-ext-quantize-tests)
else()
    message(STATUS "znet-quantize: googletest not available, extension tests skipped")
endif()
