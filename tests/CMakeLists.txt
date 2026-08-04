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

# the same reads with ZNET_MAX_READ_* set to 0; a separate binary because the
# ceilings are compile-time and the off case has to be exercised too
add_executable(znet-tests-buffer-limits-off buffer_limits_off.cc)
znet_apply_cxx_standard(znet-tests-buffer-limits-off)
target_link_libraries(znet-tests-buffer-limits-off PRIVATE gtest_main znet)

add_test(NAME buffer-limits-off-tests COMMAND znet-tests-buffer-limits-off)

# socket-free component tests: milliseconds, and a failure names one component
add_executable(znet-tests-zdt-unit zdt_unit.cc)
znet_apply_cxx_standard(znet-tests-zdt-unit)
target_link_libraries(znet-tests-zdt-unit PRIVATE gtest_main znet)

add_test(NAME zdt-unit-tests COMMAND znet-tests-zdt-unit)

add_executable(znet-tests-session session_unit.cc)
znet_apply_cxx_standard(znet-tests-session)
target_link_libraries(znet-tests-session PRIVATE gtest_main znet)

add_test(NAME session-unit-tests COMMAND znet-tests-session)

add_executable(znet-tests-p2p p2p_host.cc)
znet_apply_cxx_standard(znet-tests-p2p)
target_link_libraries(znet-tests-p2p PRIVATE gtest_main znet)

add_test(NAME p2p-host-tests COMMAND znet-tests-p2p)

add_executable(znet-tests-admission admission.cc)
znet_apply_cxx_standard(znet-tests-admission)
target_link_libraries(znet-tests-admission PRIVATE gtest_main znet)

add_test(NAME admission-tests COMMAND znet-tests-admission)

add_executable(znet-tests-unix unix_socket.cc)
znet_apply_cxx_standard(znet-tests-unix)
target_link_libraries(znet-tests-unix PRIVATE gtest_main znet)

add_test(NAME unix-socket-tests COMMAND znet-tests-unix)

add_executable(znet-tests-tcp tcp_transport.cc)
znet_apply_cxx_standard(znet-tests-tcp)
target_link_libraries(znet-tests-tcp PRIVATE gtest_main znet)

add_test(NAME tcp-transport-tests COMMAND znet-tests-tcp)

add_executable(znet-tests-locator locator.cc)
znet_apply_cxx_standard(znet-tests-locator)
target_link_libraries(znet-tests-locator PRIVATE gtest_main znet)

add_test(NAME locator-tests COMMAND znet-tests-locator)

add_executable(znet-tests-encryption encryption.cc)
znet_apply_cxx_standard(znet-tests-encryption)
target_link_libraries(znet-tests-encryption PRIVATE gtest_main znet)

add_test(NAME encryption-tests COMMAND znet-tests-encryption)

add_executable(znet-tests-zdt zdt.cc)
znet_apply_cxx_standard(znet-tests-zdt)
target_link_libraries(znet-tests-zdt PRIVATE gtest_main znet)

add_test(NAME zdt-tests COMMAND znet-tests-zdt)