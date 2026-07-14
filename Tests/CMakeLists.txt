# MafiaNet Test Suites (GoogleTest)
#
# Two binaries, both discovered by CTest via gtest_discover_tests:
#   UnitTests        (Tests/Unit)        label "unit"        — hermetic and
#     deterministic: no loopback networking, no wall-clock deadlines.
#   IntegrationTests (Tests/Integration) label "integration" — real UDP over
#     loopback. RUN_SERIAL (several tests bind fixed ports) with a per-test
#     timeout; CI retries transient timing misses (ctest --repeat until-pass:3).
#
# Under ctest every TEST() runs in its own process, so no state (sockets,
# peers, GUIDs) can bleed between tests — the historical source of CI flakes.
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

include(GoogleTest)

# Shared helpers inherited from the legacy suite (wait/connect polling,
# packet-drop plugins, timers).
file(GLOB TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Support/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Support/*.h"
)
add_library(TestSupport STATIC ${TEST_SUPPORT_SOURCES})
target_link_libraries(TestSupport PUBLIC MafiaNet::MafiaNetStatic)
target_include_directories(TestSupport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Support
    ${PROJECT_SOURCE_DIR}/Source/include
)
set_target_properties(TestSupport PROPERTIES FOLDER "Tests")

# -----------------------------------------------------------------------------
# Unit suite
# -----------------------------------------------------------------------------
file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Unit/*.cpp"
)
add_executable(UnitTests ${UNIT_TEST_SOURCES})
target_link_libraries(UnitTests PRIVATE TestSupport GTest::gtest_main)
set_target_properties(UnitTests PROPERTIES FOLDER "Tests")

gtest_discover_tests(UnitTests
    PROPERTIES LABELS unit
    DISCOVERY_TIMEOUT 60
)

# -----------------------------------------------------------------------------
# Integration suite
# -----------------------------------------------------------------------------
file(GLOB INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Integration/*.cpp"
)
add_executable(IntegrationTests ${INTEGRATION_TEST_SOURCES})
target_link_libraries(IntegrationTests PRIVATE TestSupport GTest::gtest_main)
set_target_properties(IntegrationTests PROPERTIES FOLDER "Tests")

gtest_discover_tests(IntegrationTests
    PROPERTIES LABELS integration RUN_SERIAL TRUE TIMEOUT 600
    DISCOVERY_TIMEOUT 60
)
