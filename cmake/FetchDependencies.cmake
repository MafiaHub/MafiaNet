# MafiaNet Dependency Fetching
#
# This module fetches all required dependencies using CMake's FetchContent.
# Dependencies are always fetched (no system package fallback) for reproducible builds.
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

include(FetchContent)

message(STATUS "Fetching MafiaNet dependencies...")

# -----------------------------------------------------------------------------
# jansson - JSON library for ComprehensivePCGame master server communication
# -----------------------------------------------------------------------------
FetchContent_Declare(
    jansson
    GIT_REPOSITORY https://github.com/akheron/jansson.git
    GIT_TAG        v2.15.1
    GIT_SHALLOW    TRUE
)

# jansson configuration
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
set(JANSSON_INSTALL OFF CACHE BOOL "" FORCE)

# -----------------------------------------------------------------------------
# miniupnpc - UPnP NAT traversal
# -----------------------------------------------------------------------------
FetchContent_Declare(
    miniupnpc
    GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
    GIT_TAG        miniupnpc_2_3_3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  miniupnpc
)

# miniupnpc configuration
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)

# Note: Opus and RNNoise (used by RakVoice, which is built into the core
# library) are fetched separately in FetchVoiceDependencies.cmake so they are
# always available, independent of MAFIANET_BUILD_SAMPLES.

# -----------------------------------------------------------------------------
# Fetch all dependencies
# -----------------------------------------------------------------------------
FetchContent_MakeAvailable(miniupnpc jansson)

# -----------------------------------------------------------------------------
# Set folder for fetched dependencies in IDE
# -----------------------------------------------------------------------------
if(TARGET libminiupnpc-static)
    set_target_properties(libminiupnpc-static PROPERTIES FOLDER "Dependencies")
endif()
if(TARGET jansson)
    set_target_properties(jansson PROPERTIES FOLDER "Dependencies")
endif()

message(STATUS "MafiaNet dependencies fetched successfully")
