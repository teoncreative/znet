cmake_minimum_required(VERSION 3.29)
project(chat-tui-client LANGUAGES CXX)

add_executable(chat-tui-client main.cc)
znet_apply_cxx_standard(chat-tui-client)
target_link_libraries(chat-tui-client PRIVATE
        znet
        ftxui::screen
        ftxui::dom
        ftxui::component
)
target_include_directories(chat-tui-client PRIVATE ../common)
