cmake_minimum_required(VERSION 3.25)
project(znet)

set(CMAKE_CXX_STANDARD 20)


set(ZNET_SOURCES
        src/scheduler.cpp
        src/signal_handler.cpp
        src/inet_addr.cpp
        src/connection_session.cpp
        src/server_session.cpp
        src/client_session.cpp
        src/server.cpp
        src/client.cpp
)

add_library(znet STATIC
        ${ZNET_SOURCES}
)

target_precompile_headers(znet PUBLIC include/znet/precompiled.h)
target_include_directories(znet PUBLIC include)

target_link_libraries(znet fmt)
