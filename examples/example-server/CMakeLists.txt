cmake_minimum_required(VERSION 3.16.3)
project(example-server)

set(CMAKE_CXX_STANDARD 20)

add_executable(example-server main.cc)
target_link_libraries(example-server PRIVATE znet)
include_directories(../example-common)

