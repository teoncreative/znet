cmake_minimum_required(VERSION 3.16.3)
project(basic-client)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(basic-client main.cc)
target_link_libraries(basic-client PRIVATE znet)
include_directories(../common)

