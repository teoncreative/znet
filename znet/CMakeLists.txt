cmake_minimum_required(VERSION 3.16.3)
project(znet)

option(ZNET_USE_EXTERNAL_FMT "Looks for FMT via find_package" OFF)
option(ZNET_PREFER_STD_SLEEP "Prefers thread sleep to wait between ticks, this results in less precision of the timings but reduces CPU usage." OFF)
option(ZNET_PREFER_IPV4 "Prefers IPv4 addresses." OFF)

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive -Werror=vla -Wno-deprecated")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

set(ZNET_SOURCES
        src/scheduler.cc
        src/signal_handler.cc
        src/inet_addr.cc
        src/server.cc
        src/client.cc
        src/error.cc
        src/logger.cc
        src/encryption.cc
        src/peer_session.cc
        src/compression.cc
        src/codec.cc
        src/util.cc
        src/pch.cc
        src/init.cc
        src/backend/tcp.cc
        src/p2p/locator.cc
)

add_library(znet STATIC
        ${ZNET_SOURCES}
)

set_target_properties(znet PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_precompile_headers(znet PRIVATE include/znet/precompiled.h)
target_include_directories(znet PUBLIC include)
## Defines
if(ZNET_PREFER_STD_SLEEP)
    target_compile_definitions(znet PUBLIC ZNET_PREFER_STD_SLEEP)
endif()

## Dependencies
if(WIN32)
    target_link_libraries(znet PUBLIC winmm ws2_32)
endif()

# FMT
if(ZNET_USE_EXTERNAL_FMT)
    find_package(fmt CONFIG REQUIRED)
endif()
target_link_libraries(znet PUBLIC fmt::fmt)

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(znet PUBLIC ${OPENSSL_SSL_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARIES})
target_include_directories(znet PUBLIC ${OPENSSL_INCLUDE_DIR})

# zstd
if(ZNET_USE_EXTERNAL_FMT)
    find_package(zstd::zstd CONFIG REQUIRED)

    if(TARGET zstd::zstd)
        target_link_libraries(znet PUBLIC zstd::zstd)
        target_compile_definitions(znet PUBLIC ZNET_USE_ZSTD=1)
    else()
        message(STATUS "zstd not found — compression disabled.")
    endif()
else()
    if(TARGET libzstd)
        target_link_libraries(znet PUBLIC libzstd)
        target_compile_definitions(znet PUBLIC ZNET_USE_ZSTD=1)
    else()
        message(STATUS "zstd not found — compression disabled.")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Apple")
    find_package(Threads REQUIRED)
    target_link_libraries(znet PRIVATE Threads::Threads)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(znet PRIVATE stdc++)
endif()