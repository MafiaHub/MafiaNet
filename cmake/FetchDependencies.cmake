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
# bzip2 - compression library for Autopatcher
# Note: Using master branch as it has CMake support (1.0.8 tag does not)
# -----------------------------------------------------------------------------
FetchContent_Declare(
    bzip2
    GIT_REPOSITORY https://gitlab.com/bzip2/bzip2.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# -----------------------------------------------------------------------------
# jansson - JSON library for ComprehensivePCGame master server communication
# Note: Using master branch as v2.14.1 has CMake compatibility issues with cmake_minimum_required
# -----------------------------------------------------------------------------
FetchContent_Declare(
    jansson
    GIT_REPOSITORY https://github.com/akheron/jansson.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# jansson configuration
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
set(JANSSON_INSTALL OFF CACHE BOOL "" FORCE)

# bzip2 configuration
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC_LIB ON CACHE BOOL "" FORCE)

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
FetchContent_MakeAvailable(bzip2 miniupnpc jansson)

# -----------------------------------------------------------------------------
# Create aliases for compatibility with existing target names
# -----------------------------------------------------------------------------

# bzip2 upstream creates bz2_static, we need BZip2 for Autopatcher
if(TARGET bz2_static AND NOT TARGET BZip2)
    # Add include directory for bzip2 headers (bzlib.h)
    target_include_directories(bz2_static PUBLIC ${bzip2_SOURCE_DIR})
    add_library(BZip2 ALIAS bz2_static)
endif()

# -----------------------------------------------------------------------------
# Set folder for fetched dependencies in IDE
# -----------------------------------------------------------------------------
if(TARGET bz2_static)
    set_target_properties(bz2_static PROPERTIES FOLDER "Dependencies")
endif()
if(TARGET libminiupnpc-static)
    set_target_properties(libminiupnpc-static PROPERTIES FOLDER "Dependencies")
endif()
if(TARGET jansson)
    set_target_properties(jansson PROPERTIES FOLDER "Dependencies")
endif()

message(STATUS "MafiaNet dependencies fetched successfully")
