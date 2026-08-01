cmake_minimum_required(VERSION 3.29)

# znet-glm: Buffer read/write for glm's vector, matrix and quaternion types.

option(ZNET_EXT_GLM "Build the glm extension" ON)
if(NOT ZNET_EXT_GLM)
    return()
endif()

# Prefer an installed glm: a consumer almost certainly already has the exact
# version their renderer was built against, and quietly fetching a second one
# would put two glm headers on the include path.
if(NOT TARGET glm::glm AND NOT TARGET glm)
    find_package(glm CONFIG QUIET)
endif()

if(NOT TARGET glm::glm AND NOT TARGET glm)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-glm: glm not installed, fetching it")
        include(FetchContent)
        FetchContent_Declare(
            glm
            URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
            SYSTEM
            URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
        )
        FetchContent_MakeAvailable(glm)
    else()
        message(STATUS "znet-glm: glm not found and fetching is off, skipping extension")
        return()
    endif()
endif()

# Some distributions export the bare target name rather than the namespaced one.
if(TARGET glm::glm)
    set(ZNET_GLM_TARGET glm::glm)
else()
    set(ZNET_GLM_TARGET glm)
endif()

add_library(znet-glm INTERFACE)
add_library(znet::glm ALIAS znet-glm)

target_include_directories(znet-glm INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(znet-glm INTERFACE znet znet-quantize ${ZNET_GLM_TARGET})

# Header-only, so the standard is a floor on the consumer rather than a
# property of a compiled object: the headers work at 14 and use nothing newer.
target_compile_features(znet-glm INTERFACE cxx_std_14)

message(STATUS "znet-glm: enabled against ${ZNET_GLM_TARGET}")

# Tests, only when the parent build has already set up googletest. The
# extension does not fetch a test framework of its own; a consumer vendoring
# just this directory should get the headers and nothing else.
if(TARGET gtest_main)
    add_executable(znet-ext-glm-tests
            tests/glm_serialize_test.cc
            tests/glm_quantize_test.cc
    )
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-glm-tests)
    endif()
    target_link_libraries(znet-ext-glm-tests PRIVATE gtest_main znet-glm)

    # The headers are held to the same warning bar as the core library, since
    # they end up inside a consumer's translation units where those warnings
    # are the consumer's problem.
    target_compile_options(znet-ext-glm-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-glm-tests COMMAND znet-ext-glm-tests)
else()
    message(STATUS "znet-glm: googletest not available, extension tests skipped")
endif()
