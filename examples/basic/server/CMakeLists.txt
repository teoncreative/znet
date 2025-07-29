cmake_minimum_required(VERSION 3.16.3)
project(basic-server)

add_executable(basic-server main.cc)
set_target_properties(basic-server PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_link_libraries(basic-server PRIVATE znet)
include_directories(../common)

