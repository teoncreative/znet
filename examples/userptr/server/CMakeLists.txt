cmake_minimum_required(VERSION 3.29)
project(userptr-server LANGUAGES CXX)

add_executable(userptr-server main.cc)
znet_apply_cxx_standard(userptr-server)
target_link_libraries(userptr-server PRIVATE znet)
target_include_directories(userptr-server PRIVATE ../common)
