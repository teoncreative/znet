cmake_minimum_required(VERSION 3.29)

# znet-reflect: serialize a struct from its fields, mostly without declaring them.

option(ZNET_EXT_REFLECT "Build the reflection extension" ON)
if(NOT ZNET_EXT_REFLECT)
    return()
endif()

# C++17, and it buys two distinct things. Structured bindings are what make the
# automatic aggregate walk possible at all, so on C++14 the extension could
# only ever offer the macro. `if constexpr` then collapses the type dispatch
# from a SFINAE overload set into one readable chain. znet itself still builds
# at 14; this steps aside there rather than dragging the tree up.
if(ZNET_CXX_STANDARD LESS 17)
    message(STATUS "znet-reflect: needs C++17 for structured bindings and the tree is at C++${ZNET_CXX_STANDARD}; skipping extension")
    return()
endif()

add_library(znet-reflect INTERFACE)
add_library(znet::reflect ALIAS znet-reflect)

target_include_directories(znet-reflect INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-reflect INTERFACE znet)
target_compile_features(znet-reflect INTERFACE cxx_std_17)

message(STATUS "znet-reflect: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-reflect-tests tests/reflect_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-reflect-tests)
    endif()
    target_link_libraries(znet-ext-reflect-tests PRIVATE gtest_main znet-reflect)

    target_compile_options(znet-ext-reflect-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-reflect-tests COMMAND znet-ext-reflect-tests)
else()
    message(STATUS "znet-reflect: googletest not available, extension tests skipped")
endif()
