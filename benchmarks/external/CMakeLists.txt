# Comparison targets. Each is opt-in on its own, because each downloads sources
# at configure time and they vary a lot in how cleanly they build:
#
#   -DZNET_BENCH_ENET=ON     small, clean CMake, builds anywhere
#   -DZNET_BENCH_RAKNET=ON   archived upstream; old CMake, may need a toolchain nudge
#   -DZNET_BENCH_GNS=ON      Valve GameNetworkingSockets; pulls protobuf + a crypto
#                            backend, by far the heaviest of the three
#
# Turning one off leaves the others untouched, so a library that stops building
# does not block the suite.

include(FetchContent)

option(ZNET_BENCH_ENET "Benchmark against ENet" OFF)
option(ZNET_BENCH_RAKNET "Benchmark against RakNet" OFF)
option(ZNET_BENCH_GNS "Benchmark against Valve GameNetworkingSockets" OFF)

# --- ENet ---------------------------------------------------------------------
if(ZNET_BENCH_ENET)
    FetchContent_Declare(enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG v1.3.18
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(enet)
    znet_add_benchmark(enet-bench enet_bench.cc)
    target_link_libraries(enet-bench PRIVATE enet)
    # ENet's headers live next to its sources rather than in an interface target.
    target_include_directories(enet-bench PRIVATE "${enet_SOURCE_DIR}/include")
    add_dependencies(benchmarks enet-bench)
    message(STATUS "benchmark: ENet enabled")
endif()

# --- RakNet -------------------------------------------------------------------
if(ZNET_BENCH_RAKNET)
    # Upstream RakNet is archived: no tags, only master, and its CMake declares
    # cmake_minimum_required(VERSION 2.6), which CMake 4 refuses outright. Pin
    # the last master commit and raise the policy floor just for this subtree.
    set(RAKNET_ENABLE_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RAKNET_ENABLE_DLL OFF CACHE BOOL "" FORCE)
    set(RAKNET_ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(RAKNET_GENERATE_INCLUDE_ONLY_DIR ON CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(raknet
        GIT_REPOSITORY https://github.com/facebookarchive/RakNet.git
        GIT_TAG 1a169895a900c9fc4841c556e16514182b75faf8
    )
    FetchContent_MakeAvailable(raknet)
    # RakNet's own sources predate C++11 conformance (pointer/integer
    # comparisons, missing includes), so its target is built permissively and
    # with warnings off. This applies only to the vendored library.
    if(NOT MSVC)
        target_compile_options(RakNetLibStatic PRIVATE -fpermissive -w)
    endif()
    znet_add_benchmark(raknet-bench raknet_bench.cc)
    target_link_libraries(raknet-bench PRIVATE RakNetLibStatic)
    target_include_directories(raknet-bench PRIVATE "${raknet_SOURCE_DIR}/Source")
    add_dependencies(benchmarks raknet-bench)
    message(STATUS "benchmark: RakNet enabled")
endif()

# --- GameNetworkingSockets ----------------------------------------------------
if(ZNET_BENCH_GNS)
    # GNS needs protobuf and a crypto backend present on the system; it does not
    # vendor them. If configure fails here, install those first (on Debian:
    # libprotobuf-dev protobuf-compiler libssl-dev) or leave this option off.
    # GNS compiles C sources; the rest of the project is CXX-only.
    enable_language(C)
    set(GAMENETWORKINGSOCKETS_BUILD_examples OFF CACHE BOOL "" FORCE)
    set(GAMENETWORKINGSOCKETS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(gns
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG v1.6.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(gns)
    znet_add_benchmark(gns-bench gns_bench.cc)
    target_link_libraries(gns-bench PRIVATE GameNetworkingSockets::static)
    add_dependencies(benchmarks gns-bench)
    message(STATUS "benchmark: GameNetworkingSockets enabled")
endif()

if(NOT ZNET_BENCH_ENET AND NOT ZNET_BENCH_RAKNET AND NOT ZNET_BENCH_GNS)
    message(STATUS "benchmark: no comparison libraries enabled "
                   "(-DZNET_BENCH_ENET=ON, -DZNET_BENCH_RAKNET=ON, -DZNET_BENCH_GNS=ON)")
endif()
