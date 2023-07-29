cmake_minimum_required(VERSION 3.25)
project(chat-server)

set(CMAKE_CXX_STANDARD 20)

add_executable(chat-server main.cpp user.cpp chat_server.cpp)
target_link_libraries(chat-server PRIVATE znet)
include_directories(../common)
target_precompile_headers(chat-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/precompiled.h)

