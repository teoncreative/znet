cmake_minimum_required(VERSION 3.29)
project(znet
    VERSION 2.0.0
    DESCRIPTION "Modern C++20 networking library"
    LANGUAGES CXX
)

# Options
option(ZNET_PREFER_STD_SLEEP "Prefers thread sleep to wait between ticks, this results in less precision of the timings but reduces CPU usage." OFF)
option(ZNET_PREFER_IPV4 "Prefers IPv4 addresses." OFF)
option(ZNET_ENABLE_STRICT_WARNINGS "Enable strict compiler warnings" ON)
option(ZNET_ENABLE_METRICS "Collect per-session and per-server counters" ON)

set(ZNET_SOURCES
        src/scheduler.cc
        src/signal_handler.cc
        src/inet_addr.cc
        src/server.cc
        src/client.cc
        src/error.cc
        src/logger.cc
        src/encryption.cc
        src/peer_session.cc
        src/compression.cc
        src/codec.cc
        src/util.cc
        src/pch.cc
        src/init.cc
        src/backend/backend.cc
        src/backend/tcp.cc
        src/backend/zdt.cc
        src/p2p/locator.cc
        src/p2p/dialer.cc
        src/p2p/dialer_tcp.cc
        src/p2p/dialer_zdt.cc
)

add_library(znet STATIC
        ${ZNET_SOURCES}
)

set_target_properties(znet PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF  # Disable compiler-specific extensions
)

target_precompile_headers(znet PRIVATE include/znet/precompiled.h)
target_include_directories(znet PUBLIC include)

target_compile_options(znet PUBLIC
        # MSVC Options
        $<$<CXX_COMPILER_ID:MSVC>:
        /Zc:preprocessor
        >
)

# Compiler-specific warning flags (Modern CMake style)
if(ZNET_ENABLE_STRICT_WARNINGS)
    target_compile_options(znet PRIVATE
        # MSVC warnings
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4                          # High warning level
            /analyze                     # Static analysis
            /permissive-                 # Standards conformance
            /w14242  # Conversion warnings
            /w14254  # Operator conversion
            /w14263  # Member function override
            /w14265  # Virtual destructor
            /w14287  # Unsigned/negative constant mismatch
            /we4289  # Loop variable used outside scope (error)
            /w14296  # Expression always true/false
            /w14311  # Pointer truncation
            /w14545  # Expression before comma evaluates to a function
            /w14546  # Function call before comma missing argument list
            /w14547  # Operator before comma has no effect
            /w14549  # Operator before comma has no effect
            /w14555  # Expression has no effect
            /w14619  # Pragma warning
            /w14640  # Thread-unsafe static member initialization
            /w14826  # Conversion is sign-extended
            /w14905  # String literal cast to LPSTR
            /w14906  # Wide string literal cast to LPSTR
            /w14928  # Illegal copy-initialization
            /WX
            /analyze:external-
        >

        # GCC warnings
        $<$<CXX_COMPILER_ID:GNU>:
            -Wall                        # Enable most warnings
            -Wextra                      # Extra warnings
            -Wpedantic                   # Pedantic warnings
            -Wuninitialized              # Uninitialized variables
            -Warray-bounds               # Array bounds checking
            -Wstringop-overflow          # String operation overflow
            -Wreturn-type                # Missing return statements
            -Wimplicit-fallthrough       # Implicit switch fallthrough
            -Wconversion                 # Type conversions
            -Wsign-conversion            # Sign conversions
            -Wcast-align                 # Cast alignment
            -Wold-style-cast             # C-style casts
            -Wshadow                     # Variable shadowing
            -Wnull-dereference           # Null pointer dereference
            -Wformat-security            # Format string security
            -Wdelete-non-virtual-dtor    # Non-virtual destructor delete
            -Werror=vla                  # Variable-length arrays (error)
            $<$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,12.0>:
                -Wuse-after-free         # Use-after-free (GCC 12+)
                -Wdangling-pointer       # Dangling pointers (GCC 12+)
            >
        >

        # Clang warnings
        $<$<CXX_COMPILER_ID:Clang>:
            -Wall                        # Enable most warnings
            -Wextra                      # Extra warnings
            -Wpedantic                   # Pedantic warnings
            -Wuninitialized              # Uninitialized variables
            -Wconditional-uninitialized  # Conditional uninitialized (Clang-specific)
            -Warray-bounds               # Array bounds checking
            -Wreturn-type                # Missing return statements
            -Wimplicit-fallthrough       # Implicit switch fallthrough
            -Wconversion                 # Type conversions
            -Wsign-conversion            # Sign conversions
            -Wcast-align                 # Cast alignment
            -Wold-style-cast             # C-style casts
            -Wshadow                     # Variable shadowing
            -Wnull-dereference           # Null pointer dereference
            -Wformat-security            # Format string security
            -Wdelete-non-virtual-dtor    # Non-virtual destructor delete
            -Werror=vla                  # Variable-length arrays (error)
            $<$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,15.0>:
                -Wdangling                # Dangling references (Clang 15+)
                -Wdangling-field          # Dangling fields (Clang 15+)
            >
        >

        # Common flags for GCC and Clang
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
            -Wno-deprecated              # Silence deprecated warnings (for now)
            -Werror
        >
    )
endif()

# Platform-specific flags
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(znet PRIVATE -pthread)
endif()

# Defines
# All feature flags carry an explicit 0/1 so that -DFLAG=0 disables rather than
# enables (which is what bare #ifdef would do), and so a build that never sets
# them still gets the documented default from the header.
if(ZNET_PREFER_STD_SLEEP)
    target_compile_definitions(znet PUBLIC ZNET_PREFER_STD_SLEEP=1)
else()
    target_compile_definitions(znet PUBLIC ZNET_PREFER_STD_SLEEP=0)
endif()

if(ZNET_PREFER_IPV4)
    target_compile_definitions(znet PUBLIC ZNET_PREFER_IPV4=1)
else()
    target_compile_definitions(znet PUBLIC ZNET_PREFER_IPV4=0)
endif()

if(ZNET_ENABLE_METRICS)
    target_compile_definitions(znet PUBLIC ZNET_ENABLE_METRICS=1)
else()
    target_compile_definitions(znet PUBLIC ZNET_ENABLE_METRICS=0)
endif()

## Dependencies
if(WIN32)
    target_link_libraries(znet PUBLIC winmm ws2_32)
endif()

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(znet PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# zstd
option(ZNET_USE_EXTERNAL_ZSTD "Looks for ZSTD via find_package" OFF)
if(ZNET_USE_EXTERNAL_ZSTD)
    find_package(zstd::zstd CONFIG REQUIRED)

    if(TARGET zstd::zstd)
        target_link_libraries(znet PUBLIC zstd::zstd)
        target_compile_definitions(znet PUBLIC ZNET_USE_ZSTD=1)
    else()
        message(STATUS "zstd not found — compression disabled.")
    endif()
else()
    if(TARGET libzstd)
        target_link_libraries(znet PUBLIC libzstd)
        target_compile_definitions(znet PUBLIC ZNET_USE_ZSTD=1)
    else()
        message(STATUS "zstd not found — compression disabled.")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Apple")
    find_package(Threads REQUIRED)
    target_link_libraries(znet PRIVATE Threads::Threads)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(znet PRIVATE stdc++)
endif()