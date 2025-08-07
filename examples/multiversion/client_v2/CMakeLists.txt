cmake_minimum_required(VERSION 3.16.3)
project(multiversion-client)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(multiversion-client_v2 main.cc)
target_link_libraries(multiversion-client_v2 PRIVATE znet)
include_directories(../common)

