cmake_minimum_required(VERSION 3.16.3)
project(demo-server)

set(CMAKE_CXX_STANDARD 20)

add_executable(demo-server main.cc)
target_link_libraries(demo-server PRIVATE znet)
include_directories(../demo-common)

