cmake_minimum_required(VERSION 3.16.3)
project(znet-tests)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
FetchContent_Declare(
  googletest
  # Specify the commit you depend on and update it regularly.
  URL https://github.com/google/googletest/archive/5376968f6948923e2411081fd9372e71a59d8e77.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(znet-tests-buffer buffer.cc)
target_link_libraries(znet-tests-buffer gtest_main znet)

add_test(NAME buffer-tests COMMAND znet-tests-buffer)