cmake_minimum_required(VERSION 3.25)
project(demo-client)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG -g -O0 CACHE STRING "")

add_executable(demo-client main.cc)
target_link_libraries(demo-client PRIVATE znet)
include_directories(../demo-common)

