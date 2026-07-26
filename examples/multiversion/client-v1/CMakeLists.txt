cmake_minimum_required(VERSION 3.29)
project(multiversion-client-v1 LANGUAGES CXX)

add_executable(multiversion-client-v1 main.cc)
znet_apply_cxx_standard(multiversion-client-v1)
target_link_libraries(multiversion-client-v1 PRIVATE znet)
target_include_directories(multiversion-client-v1 PRIVATE ../common)

