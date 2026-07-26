cmake_minimum_required(VERSION 3.29)
project(multiversion-client-v2 LANGUAGES CXX)

add_executable(multiversion-client-v2 main.cc)
znet_apply_cxx_standard(multiversion-client-v2)
target_link_libraries(multiversion-client-v2 PRIVATE znet)
target_include_directories(multiversion-client-v2 PRIVATE ../common)

