cmake_minimum_required(VERSION 3.29)

# znet-json: length-prefixed json in a Buffer, with the bounds that make
# parsing network-supplied json safe.

option(ZNET_EXT_JSON "Build the nlohmann-json extension" ON)
if(NOT ZNET_EXT_JSON)
    return()
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-json: nlohmann-json not installed, fetching it")
        include(FetchContent)
        # the release tarball rather than the repo one: 115 KB against tens of
        # megabytes of test fixtures, and it carries the same CMake project
        FetchContent_Declare(
            nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            SYSTEM
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        )
        set(JSON_BuildTests OFF CACHE INTERNAL "")
        FetchContent_MakeAvailable(nlohmann_json)
    else()
        message(STATUS "znet-json: nlohmann-json not found and fetching is off, skipping extension")
        return()
    endif()
endif()

add_library(znet-json INTERFACE)
add_library(znet::json ALIAS znet-json)

target_include_directories(znet-json INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-json INTERFACE znet nlohmann_json::nlohmann_json)
target_compile_features(znet-json INTERFACE cxx_std_14)

message(STATUS "znet-json: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-json-tests tests/json_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-json-tests)
    endif()
    target_link_libraries(znet-ext-json-tests PRIVATE gtest_main znet-json)

    target_compile_options(znet-ext-json-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-json-tests COMMAND znet-ext-json-tests)
else()
    message(STATUS "znet-json: googletest not available, extension tests skipped")
endif()
