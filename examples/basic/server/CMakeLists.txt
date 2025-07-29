cmake_minimum_required(VERSION 3.16.3)
project(basic-server)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(basic-server main.cc)
target_link_libraries(basic-server PRIVATE znet)
include_directories(../common)

