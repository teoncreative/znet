cmake_minimum_required(VERSION 3.29)
project(basic-p2p LANGUAGES CXX)

add_executable(basic-p2p main.cc)
znet_apply_cxx_standard(basic-p2p)
target_link_libraries(basic-p2p PRIVATE znet)

