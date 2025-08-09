cmake_minimum_required(VERSION 3.16.3)
project(relay-server)

add_executable(relay-server main.cc)
set_target_properties(relay-server PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_link_libraries(relay-server PRIVATE znet)

