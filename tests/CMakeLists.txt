cmake_minimum_required(VERSION 3.16.3)
project(znet-tests)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
  googletest
  # Specify the commit you depend on and update it regularly.
  URL https://github.com/google/googletest/archive/5376968f6948923e2411081fd9372e71a59d8e77.zip
  URL_HASH SHA256=199e68f9dff997b30d420bf23cd9a0d3f66bfee4460e2cd95084a2c45ee00f1a
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(znet-tests-buffer buffer.cc)
target_link_libraries(znet-tests-buffer gtest_main znet)

add_test(NAME buffer-tests COMMAND znet-tests-buffer)