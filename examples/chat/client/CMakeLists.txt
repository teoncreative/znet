cmake_minimum_required(VERSION 3.25)
project(chat-client)

set(CMAKE_CXX_STANDARD 20)

add_executable(chat-client main.cpp)
target_link_libraries(chat-client PRIVATE znet)
include_directories(../common)

