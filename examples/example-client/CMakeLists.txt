cmake_minimum_required(VERSION 3.16.3)
project(example-client)

set(CMAKE_CXX_STANDARD 20)

add_executable(example-client main.cc)
target_link_libraries(example-client PRIVATE znet)
include_directories(../example-common)

