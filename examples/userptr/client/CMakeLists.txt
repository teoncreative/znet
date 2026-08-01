cmake_minimum_required(VERSION 3.29)
project(userptr-client LANGUAGES CXX)

add_executable(userptr-client main.cc)
znet_apply_cxx_standard(userptr-client)
target_link_libraries(userptr-client PRIVATE znet)
target_include_directories(userptr-client PRIVATE ../common)
