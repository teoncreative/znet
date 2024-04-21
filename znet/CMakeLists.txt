cmake_minimum_required(VERSION 3.25)
project(znet)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wvla -Werror=vla")

set(ZNET_SOURCES
        src/scheduler.cc
        src/signal_handler.cc
        src/inet_addr.cc
        src/server_session.cc
        src/client_session.cc
        src/server.cc
        src/client.cc
        src/error.cc
)

add_library(znet STATIC
        ${ZNET_SOURCES}
)

target_precompile_headers(znet PUBLIC include/znet/precompiled.h)
target_include_directories(znet PUBLIC include)
target_include_directories(znet PRIVATE include/znet)

target_link_libraries(znet fmt)
if(WIN32)
    target_link_libraries(znet winmm ws2_32)
endif()
