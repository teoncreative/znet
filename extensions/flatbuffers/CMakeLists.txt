cmake_minimum_required(VERSION 3.29)

# znet-flatbuffers: verified FlatBuffers payloads in a Buffer.

option(ZNET_EXT_FLATBUFFERS "Build the FlatBuffers extension" ON)
if(NOT ZNET_EXT_FLATBUFFERS)
    return()
endif()

# The runtime headers require C++17 (string_view, if constexpr) as of v23.
if(ZNET_CXX_STANDARD LESS 17)
    message(STATUS "znet-flatbuffers: FlatBuffers requires C++17 and the tree is at C++${ZNET_CXX_STANDARD}; skipping extension")
    return()
endif()

if(NOT TARGET flatbuffers::flatbuffers AND NOT TARGET flatbuffers)
    find_package(Flatbuffers CONFIG QUIET)
endif()

if(NOT TARGET flatbuffers::flatbuffers AND NOT TARGET flatbuffers)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-flatbuffers: FlatBuffers not installed, fetching it")
        include(FetchContent)
        # Only the runtime headers are wanted. flatc is a code generator that
        # belongs in whatever step compiles your .fbs files, and building it
        # here would add minutes to a build that does not use it.
        set(FLATBUFFERS_BUILD_TESTS OFF CACHE INTERNAL "")
        set(FLATBUFFERS_BUILD_FLATC OFF CACHE INTERNAL "")
        set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE INTERNAL "")
        set(FLATBUFFERS_INSTALL OFF CACHE INTERNAL "")
        FetchContent_Declare(
            flatbuffers
            URL https://github.com/google/flatbuffers/archive/refs/tags/v25.2.10.tar.gz
            SYSTEM
            URL_HASH SHA256=b9c2df49707c57a48fc0923d52b8c73beb72d675f9d44b2211e4569be40a7421
        )
        FetchContent_MakeAvailable(flatbuffers)
        znet_ext_relax_dependency_warnings(flatbuffers flatc)
    else()
        message(STATUS "znet-flatbuffers: FlatBuffers not found and fetching is off, skipping extension")
        return()
    endif()
endif()

if(TARGET flatbuffers::flatbuffers)
    set(ZNET_FLATBUFFERS_TARGET flatbuffers::flatbuffers)
else()
    set(ZNET_FLATBUFFERS_TARGET flatbuffers)
endif()

add_library(znet-flatbuffers INTERFACE)
add_library(znet::flatbuffers ALIAS znet-flatbuffers)

target_include_directories(znet-flatbuffers INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-flatbuffers INTERFACE znet ${ZNET_FLATBUFFERS_TARGET})
target_compile_features(znet-flatbuffers INTERFACE cxx_std_17)

message(STATUS "znet-flatbuffers: enabled against ${ZNET_FLATBUFFERS_TARGET}")

if(TARGET gtest_main)
    add_executable(znet-ext-flatbuffers-tests tests/flatbuffers_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-flatbuffers-tests)
    endif()
    target_link_libraries(znet-ext-flatbuffers-tests PRIVATE gtest_main znet-flatbuffers)

    target_compile_options(znet-ext-flatbuffers-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-flatbuffers-tests COMMAND znet-ext-flatbuffers-tests)
else()
    message(STATUS "znet-flatbuffers: googletest not available, extension tests skipped")
endif()
