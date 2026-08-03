cmake_minimum_required(VERSION 3.29)
project(auth-service LANGUAGES CXX)

# A library rather than headers: the signing key lives in service.cc and is
# declared nowhere, so no client or server translation unit can reach it.
add_library(auth-service STATIC service.cc)
znet_apply_cxx_standard(auth-service)
target_link_libraries(auth-service PUBLIC znet)
target_include_directories(auth-service PUBLIC . ../common)
