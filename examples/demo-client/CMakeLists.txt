cmake_minimum_required(VERSION 3.25)
project(demo-client)

set(CMAKE_CXX_STANDARD 20)

add_executable(demo-client main.cpp)
target_link_libraries(demo-client PRIVATE znet)
include_directories(../demo-common)

