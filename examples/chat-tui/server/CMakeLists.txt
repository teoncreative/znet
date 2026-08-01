cmake_minimum_required(VERSION 3.29)
project(chat-tui-server LANGUAGES CXX)

add_executable(chat-tui-server main.cc)
znet_apply_cxx_standard(chat-tui-server)
target_link_libraries(chat-tui-server PRIVATE znet)
target_include_directories(chat-tui-server PRIVATE ../common)
