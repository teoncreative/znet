cmake_minimum_required(VERSION 3.29)

# chat-tui: a chat room server and an FTXUI client.
#
# The server is plain znet and would build anywhere. The client needs FTXUI,
# which needs C++17, so the whole example skips itself rather than failing a
# build that cannot have it. Same rule as extensions/: a missing optional
# dependency must never break the parent build.

option(ZNET_EXAMPLE_CHAT_TUI "Build the chat-tui example" ON)
if(NOT ZNET_EXAMPLE_CHAT_TUI)
    return()
endif()

if(ZNET_CXX_STANDARD LESS 17)
    message(STATUS "chat-tui: needs C++17 for FTXUI, skipping (ZNET_CXX_STANDARD=${ZNET_CXX_STANDARD})")
    return()
endif()

# Prefer an installed FTXUI over fetching a second copy.
if(NOT TARGET ftxui::component)
    find_package(ftxui CONFIG QUIET)
endif()

if(NOT TARGET ftxui::component)
    if(ZNET_EXT_ALLOW_FETCH)
        message(STATUS "chat-tui: FTXUI not installed, fetching it")
        include(FetchContent)
        FetchContent_Declare(
                ftxui
                URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
                SYSTEM
                URL_HASH SHA256=a2991cb222c944aee14397965d9f6b050245da849d8c5da7c72d112de2786b5b
        )
        set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(ftxui)
    else()
        message(STATUS "chat-tui: FTXUI not found and fetching is off, skipping example")
        return()
    endif()
endif()

add_subdirectory(server)
add_subdirectory(client)

message(STATUS "chat-tui: enabled")
