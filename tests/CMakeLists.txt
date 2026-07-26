cmake_minimum_required(VERSION 3.29)
project(znet-tests LANGUAGES CXX)

include(FetchContent)

# Use timestamp policy for FetchContent
cmake_policy(SET CMP0135 NEW)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/5376968f6948923e2411081fd9372e71a59d8e77.zip
    URL_HASH SHA256=199e68f9dff997b30d420bf23cd9a0d3f66bfee4460e2cd95084a2c45ee00f1a
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(znet-tests-buffer buffer.cc)
znet_apply_cxx_standard(znet-tests-buffer)
target_link_libraries(znet-tests-buffer PRIVATE gtest_main znet)

add_test(NAME buffer-tests COMMAND znet-tests-buffer)

add_executable(znet-tests-zdt zdt.cc)
znet_apply_cxx_standard(znet-tests-zdt)
target_link_libraries(znet-tests-zdt PRIVATE gtest_main znet)

add_test(NAME zdt-tests COMMAND znet-tests-zdt)