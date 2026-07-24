cmake_minimum_required(VERSION 3.29)
project(zdt-client LANGUAGES CXX)

add_executable(zdt-client main.cc)
set_target_properties(zdt-client PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(zdt-client PRIVATE znet)
target_include_directories(zdt-client PRIVATE ../common)
