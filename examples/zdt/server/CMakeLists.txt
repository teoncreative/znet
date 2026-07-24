cmake_minimum_required(VERSION 3.29)
project(zdt-server LANGUAGES CXX)

add_executable(zdt-server main.cc)
set_target_properties(zdt-server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(zdt-server PRIVATE znet)
target_include_directories(zdt-server PRIVATE ../common)
