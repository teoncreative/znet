cmake_minimum_required(VERSION 3.16.3)
project(znet)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
        src/packet_handler.cc
        src/encryption.cc
        src/transport.cc
        src/peer_session.cc
)

add_library(znet STATIC
        ${ZNET_SOURCES}
)

target_precompile_headers(znet PUBLIC include/znet/precompiled.h)
target_include_directories(znet PUBLIC include)
target_include_directories(znet PRIVATE include/znet)

## Dependencies

if(WIN32)
    target_link_libraries(znet PUBLIC winmm ws2_32)
endif()

# FMT
#find_package(fmt CONFIG REQUIRED)
target_link_libraries(znet PUBLIC fmt::fmt)

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(znet PUBLIC ${OPENSSL_SSL_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARIES})
target_include_directories(znet PUBLIC ${OPENSSL_INCLUDE_DIR})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
find_package(Threads REQUIRED)
target_link_libraries(znet PRIVATE Threads::Threads)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(znet PRIVATE stdc++)
endif()