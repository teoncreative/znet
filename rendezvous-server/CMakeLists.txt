cmake_minimum_required(VERSION 3.16.3)
project(rendezvous-server)

add_executable(rendezvous-server main.cc)
set_target_properties(rendezvous-server PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
)
target_link_libraries(rendezvous-server PRIVATE znet)

