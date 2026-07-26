cmake_minimum_required(VERSION 3.29)
project(multiversion-server LANGUAGES CXX)

add_executable(multiversion-server main.cc)
znet_apply_cxx_standard(multiversion-server)
target_link_libraries(multiversion-server PRIVATE znet)
target_include_directories(multiversion-server PRIVATE ../common)

