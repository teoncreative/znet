cmake_minimum_required(VERSION 3.29)

# include/znet/version.h is the single source of truth; parse it so the CMake
# project version and the one compiled into the library cannot disagree.
file(READ "${CMAKE_CURRENT_LIST_DIR}/include/znet/version.h" ZNET_VERSION_HEADER)
foreach(part MAJOR MINOR PATCH)
    if(NOT ZNET_VERSION_HEADER MATCHES "#define ZNET_VERSION_${part} ([0-9]+)")
        message(FATAL_ERROR "Could not read ZNET_VERSION_${part} from version.h")
    endif()
    set(ZNET_VERSION_${part} "${CMAKE_MATCH_1}")
endforeach()

project(znet
    VERSION ${ZNET_VERSION_MAJOR}.${ZNET_VERSION_MINOR}.${ZNET_VERSION_PATCH}
    DESCRIPTION "Modern C++14/17/20 networking library"
    LANGUAGES CXX
)
message(STATUS "znet version ${PROJECT_VERSION}")

# C++ standard. znet compiles as C++14 and upwards; where a newer standard
# offers a better spelling of something, headers detect it and use it,
# so raising this only ever adds capability.
set(ZNET_CXX_STANDARD "20" CACHE STRING "C++ standard used to build znet (14, 17, 20 or 23)")
set_property(CACHE ZNET_CXX_STANDARD PROPERTY STRINGS 14 17 20 23)
if(NOT ZNET_CXX_STANDARD MATCHES "^(14|17|20|23)$")
    message(FATAL_ERROR "ZNET_CXX_STANDARD must be one of 14, 17, 20, 23 (got '${ZNET_CXX_STANDARD}')")
endif()
message(STATUS "znet C++ standard: ${ZNET_CXX_STANDARD}")

# Options
option(ZNET_PREFER_STD_SLEEP "Prefers thread sleep to wait between ticks, this results in less precision of the timings but reduces CPU usage." OFF)
option(ZNET_PREFER_IPV4 "Prefers IPv4 addresses." OFF)
option(ZNET_ENABLE_STRICT_WARNINGS "Enable strict compiler warnings" ON)
option(ZNET_ENABLE_METRICS "Collect per-session and per-server counters" ON)

# Ceilings on any length a peer puts on the wire. Both are counts, not bytes,
# and 0 removes the ceiling. A read is still refused when the bytes present
# could not back the count it claims, which needs no configuration.
set(ZNET_MAX_READ_ELEMENTS 65536 CACHE STRING "Largest element count a vector/map/array read will accept (0 = no ceiling)")
set(ZNET_MAX_READ_STRING_LENGTH 65536 CACHE STRING "Longest string a read will accept, in bytes (0 = no ceiling)")

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
        src/message_pipeline.cc
        src/session_encoder.cc
        src/compression.cc
        src/codec.cc
        src/util.cc
        src/pch.cc
        src/init.cc
        src/version.cc
        src/backend/backend.cc
        src/backend/tcp.cc
        src/backend/zdt/zdt_ack_history.cc
        src/backend/zdt/zdt_congestion.cc
        src/backend/zdt/zdt_wire.cc
        src/backend/zdt/zdt_net.cc
        src/backend/zdt/zdt_transport.cc
        src/backend/zdt/zdt_backends.cc
        src/p2p/locator.cc
        src/p2p/dialer.cc
        src/p2p/dialer_tcp.cc
        src/p2p/dialer_zdt.cc
)

add_library(znet STATIC
        ${ZNET_SOURCES}
)

set_target_properties(znet PROPERTIES
    CXX_STANDARD ${ZNET_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF  # Disable compiler-specific extensions
)

# Link-time optimization, optimized configurations only.
#
# The hot paths cross translation units: the send pipeline runs through the
# codec, compression and encryption in three files, and ZDT's flush loop calls
# helpers that used to sit in the same .cc as their caller. Without this,
# splitting a file by responsibility costs inlining that the compiler was doing
# for free, which would make every such split a throughput decision rather than
# a readability one.
#
# Checked rather than assumed: some toolchains in the CI matrix report IPO
# unsupported, and failing the build there would be worse than the lost
# inlining. Debug is left alone so it stays fast to build and to step through.
option(ZNET_ENABLE_LTO "Use link-time optimization in optimized builds" ON)
if(ZNET_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ZNET_IPO_SUPPORTED OUTPUT ZNET_IPO_MESSAGE)
    if(ZNET_IPO_SUPPORTED)
        set_target_properties(znet PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON
        )
        message(STATUS "znet: link-time optimization enabled for optimized builds")
    else()
        message(STATUS "znet: link-time optimization unavailable: ${ZNET_IPO_MESSAGE}")
    endif()
endif()

# Consumers must compile at C++14 or newer; compat.h enforces this too, but
# saying it here gives a CMake-level error instead of a preprocessor one.
target_compile_features(znet PUBLIC cxx_std_14)

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

foreach(limit ZNET_MAX_READ_ELEMENTS ZNET_MAX_READ_STRING_LENGTH)
    if(NOT ${limit} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${limit} must be a non-negative integer, got '${${limit}}'")
    endif()
    target_compile_definitions(znet PUBLIC ${limit}=${${limit}})
endforeach()

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
        message(STATUS "zstd not found, compression disabled.")
    endif()
else()
    if(TARGET libzstd)
        target_link_libraries(znet PUBLIC libzstd)
        target_compile_definitions(znet PUBLIC ZNET_USE_ZSTD=1)
    else()
        message(STATUS "zstd not found, compression disabled.")
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(znet PRIVATE Threads::Threads)
