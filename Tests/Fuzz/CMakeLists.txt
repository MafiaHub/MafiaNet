# Fuzz harnesses. Each <name>_fuzz.cpp defines LLVMFuzzerTestOneInput and is
# built two ways:
#
#   <name>_replay  Always, with the tests. Runs the harness over the checked-in
#                  corpus (corpus/<name>/: seeds plus minimized crash
#                  reproducers) as a ctest case labelled `unit`, on every
#                  compiler. A fixed crash stays fixed everywhere.
#   <name>_fuzz    With MAFIANET_BUILD_FUZZERS=ON and Clang: a libFuzzer
#                  binary. Use the `fuzz` preset (ASan + UBSan):
#                    cmake --workflow --preset fuzz
#                    ./build/fuzz/Tests/Fuzz/bit_stream_fuzz Tests/Fuzz/corpus/bit_stream -max_total_time=60
#
# Adding a harness: drop <name>_fuzz.cpp here and create corpus/<name>/.

file(GLOB MAFIANET_FUZZ_HARNESSES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_fuzz.cpp")

# Harnesses that drive whole peers use the simulated network from Tests/Support.
set(MAFIANET_FUZZ_SUPPORT
    "${CMAKE_CURRENT_SOURCE_DIR}/../Support/SimulatedNetwork.cpp"
)

if(MAFIANET_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MAFIANET_BUILD_FUZZERS needs Clang (libFuzzer); compiler is ${CMAKE_CXX_COMPILER_ID}")
endif()

foreach(harness_source IN LISTS MAFIANET_FUZZ_HARNESSES)
    get_filename_component(harness "${harness_source}" NAME_WE)
    string(REGEX REPLACE "_fuzz$" "" corpus_name "${harness}")
    set(corpus_dir "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus_name}")

    if(MAFIANET_BUILD_TESTS)
        add_executable(${corpus_name}_replay "${harness_source}" replay_main.cpp ${MAFIANET_FUZZ_SUPPORT})
        target_include_directories(${corpus_name}_replay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../Support")
        target_link_libraries(${corpus_name}_replay PRIVATE MafiaNet::MafiaNetStatic)
        target_compile_definitions(${corpus_name}_replay PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
        set_target_properties(${corpus_name}_replay PROPERTIES FOLDER "Tests/Fuzz")
        mafianet_enable_warnings(${corpus_name}_replay)
        add_test(NAME FuzzCorpus.${corpus_name} COMMAND ${corpus_name}_replay "${corpus_dir}")
        set_tests_properties(FuzzCorpus.${corpus_name} PROPERTIES LABELS unit TIMEOUT 600)
    endif()

    if(MAFIANET_BUILD_FUZZERS)
        add_executable(${harness} "${harness_source}" ${MAFIANET_FUZZ_SUPPORT})
        target_include_directories(${harness} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../Support")
        target_link_libraries(${harness} PRIVATE MafiaNet::MafiaNetStatic)
        target_compile_options(${harness} PRIVATE -fsanitize=fuzzer)
        target_link_options(${harness} PRIVATE -fsanitize=fuzzer)
        set_target_properties(${harness} PROPERTIES FOLDER "Tests/Fuzz")
        mafianet_enable_warnings(${harness})
    endif()
endforeach()
