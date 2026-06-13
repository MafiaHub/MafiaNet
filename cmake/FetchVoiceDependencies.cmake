# MafiaNet Voice Dependency Fetching
#
# Fetches the Opus codec and RNNoise noise suppression used by RakVoice.
# RakVoice is built into the core MafiaNet library, so these dependencies are
# always fetched (no system package fallback) for reproducible builds.
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

include(FetchContent)

message(STATUS "Fetching MafiaNet voice dependencies (Opus, RNNoise)...")

# -----------------------------------------------------------------------------
# Opus - audio codec for RakVoice
# -----------------------------------------------------------------------------
FetchContent_Declare(
    Opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.6.1
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
# Fetch dependencies
# -----------------------------------------------------------------------------
FetchContent_MakeAvailable(Opus)

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
        # Weight parser (parse_weights -> rnn_parse_weights via nnet.h)
        ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
        # Model weight data (rnnoise_arrays, init_rnnoise)
        ${rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c
    )
    # Get Opus source directory for celt headers (needed by vec_neon.h)
    FetchContent_GetProperties(Opus)
    target_include_directories(rnnoise
        PUBLIC
            # Build-interface only: rnnoise is bundled into the MafiaNet export,
            # but its headers are not installed (RakVoice.h forward-declares the
            # RNNoise types), so this build-tree path must not leak into the
            # exported/installed interface.
            $<BUILD_INTERFACE:${rnnoise_SOURCE_DIR}/include>
        PRIVATE
            ${rnnoise_SOURCE_DIR}/src
            ${rnnoise_model_SOURCE_DIR}/src  # Contains rnnoise_data.h
            ${opus_SOURCE_DIR}/celt           # Contains os_support.h
            ${opus_SOURCE_DIR}/include        # Contains opus_defines.h
    )
    target_compile_definitions(rnnoise PRIVATE COMPILE_OPUS=1)
    # Built static but linked into the shared libmafianet.so, so every object
    # must be position-independent (otherwise: "relocation R_X86_64_PC32 ...
    # can not be used when making a shared object; recompile with -fPIC").
    set_target_properties(rnnoise PROPERTIES
        FOLDER "Dependencies"
        POSITION_INDEPENDENT_CODE ON
    )
endif()

if(TARGET opus)
    # Same reason as rnnoise above: PIC is required to link the static opus
    # archive into the shared MafiaNet library.
    set_target_properties(opus PROPERTIES
        FOLDER "Dependencies"
        POSITION_INDEPENDENT_CODE ON
    )
endif()

message(STATUS "MafiaNet voice dependencies fetched successfully")
