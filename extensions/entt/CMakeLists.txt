cmake_minimum_required(VERSION 3.29)

# znet-entt: Buffer-backed archives for EnTT's snapshot machinery.

option(ZNET_EXT_ENTT "Build the EnTT extension" ON)
if(NOT ZNET_EXT_ENTT)
    return()
endif()

# EnTT declares cxx_std_17 and uses fold expressions, if-init and
# std::is_const_v throughout. znet itself still builds at 14, so rather than
# let a C++14 tree fail on a dependency's requirement, this extension steps
# aside and says why.
if(ZNET_CXX_STANDARD LESS 17)
    message(STATUS "znet-entt: EnTT requires C++17 and the tree is at C++${ZNET_CXX_STANDARD}; skipping extension")
    return()
endif()

if(NOT TARGET EnTT::EnTT)
    find_package(EnTT CONFIG QUIET)
endif()

if(NOT TARGET EnTT::EnTT)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-entt: EnTT not installed, fetching it")
        include(FetchContent)
        FetchContent_Declare(
            EnTT
            URL https://github.com/skypjack/entt/archive/refs/tags/v3.15.0.tar.gz
            SYSTEM
            URL_HASH SHA256=01466fcbf77618a79b62891510c0bbf25ac2804af5751c84982b413852234d66
        )
        FetchContent_MakeAvailable(EnTT)
    else()
        message(STATUS "znet-entt: EnTT not found and fetching is off, skipping extension")
        return()
    endif()
endif()

add_library(znet-entt INTERFACE)
add_library(znet::entt ALIAS znet-entt)

target_include_directories(znet-entt INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-entt INTERFACE znet EnTT::EnTT)
target_compile_features(znet-entt INTERFACE cxx_std_17)

message(STATUS "znet-entt: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-entt-tests tests/entt_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-entt-tests)
    endif()
    target_link_libraries(znet-ext-entt-tests PRIVATE gtest_main znet-entt)

    target_compile_options(znet-ext-entt-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-entt-tests COMMAND znet-ext-entt-tests)
else()
    message(STATUS "znet-entt: googletest not available, extension tests skipped")
endif()
