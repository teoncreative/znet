cmake_minimum_required(VERSION 3.29)
project(auth-server LANGUAGES CXX)

add_executable(auth-server main.cc)
znet_apply_cxx_standard(auth-server)
target_link_libraries(auth-server PRIVATE znet auth-service)
