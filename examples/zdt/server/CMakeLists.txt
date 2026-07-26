cmake_minimum_required(VERSION 3.29)
project(zdt-server LANGUAGES CXX)

add_executable(zdt-server main.cc)
znet_apply_cxx_standard(zdt-server)
target_link_libraries(zdt-server PRIVATE znet)
target_include_directories(zdt-server PRIVATE ../common)
