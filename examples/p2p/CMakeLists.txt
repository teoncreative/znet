cmake_minimum_required(VERSION 3.16.3)
project(basic-server)

add_executable(basic-p2p main.cc)
set_target_properties(basic-p2p PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_link_libraries(basic-p2p PRIVATE znet)

