# MafiaNet Unit Tests (GoogleTest)
#
# Deterministic, hermetic unit tests: no loopback networking, no wall-clock
# deadlines, no shared ports. Timing-sensitive integration tests live in
# Samples/Tests and are registered with CTest there.
#
# Copyright (c) 2026, MafiaHub
# Licensed under MIT-style license

include(FetchContent)

# -----------------------------------------------------------------------------
# GoogleTest - unit testing framework
# -----------------------------------------------------------------------------
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)

# GoogleTest configuration
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

add_executable(UnitTests
    Unit/GuidUtilTests.cpp
    Unit/PointGridSectorizerTests.cpp
)

target_link_libraries(UnitTests PRIVATE
    MafiaNet::MafiaNetStatic
    GTest::gtest_main
)

target_include_directories(UnitTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Source/include
)

set_target_properties(UnitTests PROPERTIES FOLDER "Tests")

include(GoogleTest)
# Each TEST() becomes its own CTest test, labeled "unit" so CI can select the
# hermetic suite alone (e.g. on Windows, where the integration suite is not
# yet validated): ctest -L unit
gtest_discover_tests(UnitTests
    PROPERTIES LABELS unit
    DISCOVERY_TIMEOUT 60
)
