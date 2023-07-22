cmake_minimum_required(VERSION 3.25)
project(znet)

set(CMAKE_CXX_STANDARD 20)


set(ZNET_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/inet_addr.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/connection_session.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/server_session.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client_session.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/server.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp
        )

add_library(znet STATIC
        ${ZNET_SOURCES}
        )

target_precompile_headers(znet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/znet/precompiled.h)
target_include_directories(znet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(znet fmt)
