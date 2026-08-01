cmake_minimum_required(VERSION 3.29)

# znet-bullet: Bullet rigid body state on the wire.

option(ZNET_EXT_BULLET "Build the Bullet extension" ON)
if(NOT ZNET_EXT_BULLET)
    return()
endif()

if(NOT TARGET znet-bitpack)
    message(STATUS "znet-bullet: needs znet-bitpack, which is off; skipping extension")
    return()
endif()

if(NOT TARGET LinearMath)
    find_package(Bullet CONFIG QUIET)
    if(BULLET_INCLUDE_DIRS)
        set(ZNET_BULLET_INCLUDE "${BULLET_INCLUDE_DIRS}")
    endif()
endif()

if(NOT TARGET LinearMath)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "znet-bullet: Bullet not installed, fetching it (136 MB)")
        include(FetchContent)
        # Only LinearMath's vector and quaternion types are wanted. The demos
        # and extras are the bulk of the 136 MB and none of the use.
        set(BUILD_BULLET2_DEMOS OFF CACHE INTERNAL "")
        set(BUILD_CPU_DEMOS OFF CACHE INTERNAL "")
        set(BUILD_OPENGL3_DEMOS OFF CACHE INTERNAL "")
        set(BUILD_EXTRAS OFF CACHE INTERNAL "")
        set(BUILD_UNIT_TESTS OFF CACHE INTERNAL "")
        set(BUILD_PYBULLET OFF CACHE INTERNAL "")
        set(INSTALL_LIBS OFF CACHE INTERNAL "")
        FetchContent_Declare(
            bullet3
            URL https://github.com/bulletphysics/bullet3/archive/refs/tags/3.25.tar.gz
            SYSTEM
            URL_HASH SHA256=c45afb6399e3f68036ddb641c6bf6f552bf332d5ab6be62f7e6c54eda05ceb77
        )
        # Bullet 3.25 declares cmake_minimum_required(VERSION 2.4.3), and
        # CMake 4 removed compatibility with anything below 3.5. This is the
        # documented escape hatch; it is scoped to the fetch so it cannot
        # loosen policy for znet's own targets.
        set(ZNET_SAVED_POLICY_MIN "${CMAKE_POLICY_VERSION_MINIMUM}")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        FetchContent_MakeAvailable(bullet3)
        set(CMAKE_POLICY_VERSION_MINIMUM "${ZNET_SAVED_POLICY_MIN}")
        # Bullet's targets predate usage requirements and carry no interface
        # include directory, so consumers have to be told where the headers are.
        set(ZNET_BULLET_INCLUDE "${bullet3_SOURCE_DIR}/src")
    else()
        message(STATUS "znet-bullet: Bullet not found and fetching is off, skipping extension")
        return()
    endif()
endif()

if(NOT TARGET LinearMath)
    message(STATUS "znet-bullet: Bullet did not provide the LinearMath target, skipping extension")
    return()
endif()

add_library(znet-bullet INTERFACE)
add_library(znet::bullet ALIAS znet-bullet)

target_include_directories(znet-bullet INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
if(ZNET_BULLET_INCLUDE)
    target_include_directories(znet-bullet SYSTEM INTERFACE "${ZNET_BULLET_INCLUDE}")
endif()
target_link_libraries(znet-bullet INTERFACE znet znet-quantize znet-bitpack LinearMath)
target_compile_features(znet-bullet INTERFACE cxx_std_14)

message(STATUS "znet-bullet: enabled")

if(TARGET gtest_main)
    add_executable(znet-ext-bullet-tests tests/bullet_test.cc)
    if(COMMAND znet_apply_cxx_standard)
        znet_apply_cxx_standard(znet-ext-bullet-tests)
    endif()
    target_link_libraries(znet-ext-bullet-tests PRIVATE gtest_main znet-bullet)

    target_compile_options(znet-ext-bullet-tests PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wold-style-cast -Wcast-align
            >
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    add_test(NAME ext-bullet-tests COMMAND znet-ext-bullet-tests)
else()
    message(STATUS "znet-bullet: googletest not available, extension tests skipped")
endif()
