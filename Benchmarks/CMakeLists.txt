# Microbenchmarks (google/benchmark). Not tests: nothing here asserts, and the
# numbers only mean something compared against a baseline on the same machine.
#
#   cmake --preset bench && cmake --build --preset bench
#   ./build/bench/Benchmarks/MafiaNetBenchmarks --benchmark_filter=BitStream
#
# Compare two builds with google/benchmark's tools/compare.py, using
# --benchmark_out=<file> --benchmark_out_format=json from each.

include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.5
    GIT_SHALLOW    TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

file(GLOB MAFIANET_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_bench.cpp")
add_executable(MafiaNetBenchmarks ${MAFIANET_BENCHMARK_SOURCES})
target_link_libraries(MafiaNetBenchmarks PRIVATE MafiaNet::MafiaNetStatic benchmark::benchmark_main)
# Must match the library: some classes carry _DEBUG-only members.
target_compile_definitions(MafiaNetBenchmarks PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
set_target_properties(MafiaNetBenchmarks PROPERTIES FOLDER "Benchmarks")
mafianet_enable_warnings(MafiaNetBenchmarks)
