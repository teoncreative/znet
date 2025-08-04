cmake_minimum_required(VERSION 3.16.3)
project(multiversion-server)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(multiversion-server main.cc)
target_link_libraries(multiversion-server PRIVATE znet)
include_directories(../common)

