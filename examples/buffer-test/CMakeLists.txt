cmake_minimum_required(VERSION 3.25)
project(buffer-test)

set(CMAKE_CXX_STANDARD 20)

add_executable(buffer-test main.cpp)
target_link_libraries(buffer-test PRIVATE znet)

