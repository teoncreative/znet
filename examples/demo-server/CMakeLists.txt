cmake_minimum_required(VERSION 3.25)
project(demo-server)

set(CMAKE_CXX_STANDARD 20)

add_executable(demo-server main.cpp)
target_link_libraries(demo-server PRIVATE znet)
include_directories(../demo-common)

