cmake_minimum_required(VERSION 3.16.3)
project(basic-client)


add_executable(basic-client main.cc)
set_target_properties(basic-client PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_link_libraries(basic-client PRIVATE znet)
include_directories(../common)

