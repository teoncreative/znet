cmake_minimum_required(VERSION 3.29)
project(rendezvous-server LANGUAGES CXX)

add_executable(rendezvous-server main.cc)
znet_apply_cxx_standard(rendezvous-server)
target_link_libraries(rendezvous-server PRIVATE znet)

