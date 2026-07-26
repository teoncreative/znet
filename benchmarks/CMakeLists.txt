cmake_minimum_required(VERSION 3.29)
project(znet-benchmarks LANGUAGES CXX)

# Benchmarks are off by default: they are slow to run, and the comparison
# targets below download third-party sources at configure time.
#
#   cmake -B build -DZNET_BUILD_BENCHMARKS=ON
#   cmake --build build --target znet-bench
#   ./build/benchmarks/znet-bench
#
# Comparisons are opt-in individually so one library failing to build does not
# take the rest with it:
#
#   -DZNET_BENCH_ENET=ON -DZNET_BENCH_RAKNET=ON -DZNET_BENCH_GNS=ON

function(znet_add_benchmark name)
    add_executable(${name} ${ARGN})
    znet_apply_cxx_standard(${name})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    # the benchmarks root, not the caller's directory, so targets defined in
    # external/ still resolve "common/harness.h".
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
endfunction()

# znet itself, and a raw-socket floor to measure it against.
znet_add_benchmark(znet-bench znet_bench.cc)
target_link_libraries(znet-bench PRIVATE znet)

znet_add_benchmark(baseline-bench baseline_bench.cc)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_package(Threads REQUIRED)
    target_link_libraries(baseline-bench PRIVATE Threads::Threads)
endif()

# Declared before the subdirectory so the comparison targets can attach
# themselves to it with add_dependencies().
add_custom_target(benchmarks DEPENDS znet-bench baseline-bench)

add_subdirectory(external)
