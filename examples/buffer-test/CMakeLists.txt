cmake_minimum_required(VERSION 3.16.3)
project(buffer-test)

set(CMAKE_CXX_STANDARD 20)

add_executable(buffer-test main.cc)
target_link_libraries(buffer-test PRIVATE znet)

