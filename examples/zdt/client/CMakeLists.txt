cmake_minimum_required(VERSION 3.29)
project(zdt-client LANGUAGES CXX)

add_executable(zdt-client main.cc)
znet_apply_cxx_standard(zdt-client)
target_link_libraries(zdt-client PRIVATE znet)
target_include_directories(zdt-client PRIVATE ../common)
