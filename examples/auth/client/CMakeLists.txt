cmake_minimum_required(VERSION 3.29)
project(auth-client LANGUAGES CXX)

add_executable(auth-client main.cc)
znet_apply_cxx_standard(auth-client)
target_link_libraries(auth-client PRIVATE znet auth-service)
