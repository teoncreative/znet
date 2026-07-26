cmake_minimum_required(VERSION 3.29)
project(basic-server LANGUAGES CXX)

add_executable(basic-server main.cc)
znet_apply_cxx_standard(basic-server)
target_link_libraries(basic-server PRIVATE znet)
target_include_directories(basic-server PRIVATE ../common)

