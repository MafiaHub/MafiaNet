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
    GIT_TAG        miniupnpc_2_2_8
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  miniupnpc
)

# miniupnpc configuration
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)

# -----------------------------------------------------------------------------
# Opus - audio codec for RakVoice
# -----------------------------------------------------------------------------
FetchContent_Declare(
    Opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
)

# Opus configuration
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

# -----------------------------------------------------------------------------
# RNNoise - neural network noise suppression for RakVoice
# -----------------------------------------------------------------------------
# RNNoise doesn't have a proper CMake build, so we fetch and build manually
FetchContent_Declare(
    rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG        v0.2
    GIT_SHALLOW    TRUE
)

# RNNoise model data (neural network weights)
set(RNNOISE_MODEL_VERSION "0b50c45")
FetchContent_Declare(
    rnnoise_model
    URL      https://media.xiph.org/rnnoise/models/rnnoise_data-${RNNOISE_MODEL_VERSION}.tar.gz
    URL_HASH SHA256=4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43
)

# -----------------------------------------------------------------------------
# Fetch all dependencies
# -----------------------------------------------------------------------------
FetchContent_MakeAvailable(bzip2 miniupnpc Opus jansson)

# Fetch rnnoise separately (needs manual target creation)
FetchContent_GetProperties(rnnoise)
if(NOT rnnoise_POPULATED)
    FetchContent_Populate(rnnoise)
endif()

# Fetch rnnoise model data
FetchContent_GetProperties(rnnoise_model)
if(NOT rnnoise_model_POPULATED)
    FetchContent_Populate(rnnoise_model)
endif()

# -----------------------------------------------------------------------------
# RNNoise - manual target creation (no upstream CMakeLists.txt)
# -----------------------------------------------------------------------------
if(NOT TARGET rnnoise)
    add_library(rnnoise STATIC
        ${rnnoise_SOURCE_DIR}/src/denoise.c
        ${rnnoise_SOURCE_DIR}/src/rnn.c
        ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c
        ${rnnoise_SOURCE_DIR}/src/nnet.c
        ${rnnoise_SOURCE_DIR}/src/nnet_default.c
        ${rnnoise_SOURCE_DIR}/src/pitch.c
        ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
        ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    )
    # Get Opus source directory for celt headers (needed by vec_neon.h)
    FetchContent_GetProperties(Opus)
    target_include_directories(rnnoise
        PUBLIC
            ${rnnoise_SOURCE_DIR}/include
        PRIVATE
            ${rnnoise_SOURCE_DIR}/src
            ${rnnoise_model_SOURCE_DIR}/src  # Contains rnnoise_data.h
            ${opus_SOURCE_DIR}/celt           # Contains os_support.h
            ${opus_SOURCE_DIR}/include        # Contains opus_defines.h
    )
    target_compile_definitions(rnnoise PRIVATE COMPILE_OPUS=1)
    set_target_properties(rnnoise PROPERTIES FOLDER "Dependencies")
endif()

# -----------------------------------------------------------------------------
# Create aliases for compatibility with existing target names
# -----------------------------------------------------------------------------

# bzip2 upstream creates bz2_static, we need BZip2 for Autopatcher
if(TARGET bz2_static AND NOT TARGET BZip2)
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
if(TARGET opus)
    set_target_properties(opus PROPERTIES FOLDER "Dependencies")
endif()
if(TARGET jansson)
    set_target_properties(jansson PROPERTIES FOLDER "Dependencies")
endif()

message(STATUS "MafiaNet dependencies fetched successfully")
