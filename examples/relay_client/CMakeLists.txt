cmake_minimum_required(VERSION 3.16.3)
project(relay-client)

add_executable(relay-client main.cc)
set_target_properties(relay-client PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_link_libraries(relay-client PRIVATE znet)

