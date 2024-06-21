cmake_minimum_required(VERSION 3.16.3)
project(example-client)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG -g -O0 CACHE STRING "")

add_executable(example-client main.cc)
target_link_libraries(example-client PRIVATE znet)
include_directories(../example-common)

