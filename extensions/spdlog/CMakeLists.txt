cmake_minimum_required(VERSION 3.29)

# znet-spdlog: route znet's logging into spdlog, severity intact.

option(ZNET_EXT_SPDLOG "Build the spdlog extension" ON)
if(NOT ZNET_EXT_SPDLOG)
    return()
endif()

if(NOT TARGET spdlog::spdlog AND NOT TARGET spdlog::spdlog_header_only)
    find_package(spdlog CONFIG QUIET)
endif()

if(NOT TARGET spdlog::spdlog AND NOT TARGET spdlog::spdlog_header_only)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-spdlog: spdlog not installed, fetching it")
        include(FetchContent)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE INTERNAL "")
        set(SPDLOG_BUILD_TESTS OFF CACHE INTERNAL "")
        set(SPDLOG_INSTALL OFF CACHE INTERNAL "")
        FetchContent_Declare(
            spdlog
            URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.1.tar.gz
            SYSTEM
            URL_HASH SHA256=25c843860f039a1600f232c6eb9e01e6627f7d030a2ae5e232bdd3c9205d26cc
        )
        FetchContent_MakeAvailable(spdlog)
        znet_ext_relax_dependency_warnings(spdlog)
    else()
        message(STATUS "znet-spdlog: spdlog not found and fetching is off, skipping extension")
        return()
    endif()
endif()

if(TARGET spdlog::spdlog)
    set(ZNET_SPDLOG_TARGET spdlog::spdlog)
else()
    set(ZNET_SPDLOG_TARGET spdlog::spdlog_header_only)
endif()

add_library(znet-spdlog INTERFACE)
add_library(znet::spdlog ALIAS znet-spdlog)

target_include_directories(znet-spdlog INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-spdlog INTERFACE znet ${ZNET_SPDLOG_TARGET})
target_compile_features(znet-spdlog INTERFACE cxx_std_14)

message(STATUS "znet-spdlog: enabled against ${ZNET_SPDLOG_TARGET}")

if(TARGET gtest_main)
    add_executable(znet-ext-spdlog-tests tests/spdlog_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-spdlog-tests)
    endif()
    target_link_libraries(znet-ext-spdlog-tests PRIVATE gtest_main znet-spdlog)

    target_compile_options(znet-ext-spdlog-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-spdlog-tests COMMAND znet-ext-spdlog-tests)
else()
    message(STATUS "znet-spdlog: googletest not available, extension tests skipped")
endif()
