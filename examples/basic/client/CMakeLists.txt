cmake_minimum_required(VERSION 3.29)
project(basic-client LANGUAGES CXX)

add_executable(basic-client main.cc)
znet_apply_cxx_standard(basic-client)
target_link_libraries(basic-client PRIVATE znet)
target_include_directories(basic-client PRIVATE ../common)

