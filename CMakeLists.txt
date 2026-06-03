# MafiaNet - Cross-platform network engine for multiplayer games
#
# Originally based on RakNet 4.082 and MafiaNet
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

cmake_minimum_required(VERSION 3.21)

project(MafiaNet
    VERSION 0.7.0
    DESCRIPTION "Cross-platform network engine for multiplayer games"
    HOMEPAGE_URL "https://github.com/mafiahub/MafiaNet"
    LANGUAGES C CXX
)

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Create a build directory and run cmake from there.")
endif()

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use folders in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Options
option(MAFIANET_BUILD_SHARED "Build shared library" ON)
option(MAFIANET_BUILD_STATIC "Build static library" ON)
option(MAFIANET_BUILD_SAMPLES "Build sample applications" OFF)
option(MAFIANET_BUILD_TESTS "Build test suite" OFF)

# Sanitizer support (Clang/GCC only).
# Set MAFIANET_SANITIZER to one of: address, undefined, address+undefined
# Example: cmake -DMAFIANET_SANITIZER=address+undefined ...
set(MAFIANET_SANITIZER "" CACHE STRING "Enable compiler sanitizers (address, undefined, address+undefined)")
if(MAFIANET_SANITIZER)
    # Parse the "+" separator: "address+undefined" -> "-fsanitize=address,undefined"
    string(REPLACE "+" "," _san_flags "${MAFIANET_SANITIZER}")
    set(_san_compile_flags "-fsanitize=${_san_flags} -fno-omit-frame-pointer -g")
    set(_san_link_flags    "-fsanitize=${_san_flags}")
    message(STATUS "Sanitizer build: -fsanitize=${_san_flags}")
    add_compile_options(-fsanitize=${_san_flags} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_san_flags})
endif()

# Ensure at least one library type is built
if(NOT MAFIANET_BUILD_SHARED AND NOT MAFIANET_BUILD_STATIC)
    message(FATAL_ERROR "At least one of MAFIANET_BUILD_SHARED or MAFIANET_BUILD_STATIC must be ON")
endif()

# Dependencies
find_package(OpenSSL 3.0.0 REQUIRED)
find_package(Threads REQUIRED)

# Include helpers
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MafiaNetHelpers)

# Platform-specific settings
if(WIN32)
    add_compile_definitions(
        _CRT_SECURE_NO_DEPRECATE
        _CRT_NONSTDC_NO_DEPRECATE
    )
endif()

# Fetch libsodium (required by core library for transport encryption)
include(FetchContent)
FetchContent_Declare(
    libsodium
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG        master  # TODO: pin to a specific commit SHA for reproducibility
    GIT_SHALLOW    TRUE
)
set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsodium)

# Library subdirectory
add_subdirectory(Source)

# Samples and extensions
if(MAFIANET_BUILD_SAMPLES)
    # Fetch dependencies (bzip2, miniupnpc, opus, rnnoise)
    include(FetchDependencies)
    add_subdirectory(Samples)
    add_subdirectory(DependentExtensions)
endif()
