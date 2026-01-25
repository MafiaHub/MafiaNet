# MafiaNet - Cross-platform network engine for multiplayer games
#
# Originally based on RakNet 4.082 and MafiaNet
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

cmake_minimum_required(VERSION 3.21)

project(MafiaNet
    VERSION 0.2.0
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

# Ensure at least one library type is built
if(NOT MAFIANET_BUILD_SHARED AND NOT MAFIANET_BUILD_STATIC)
    message(FATAL_ERROR "At least one of MAFIANET_BUILD_SHARED or MAFIANET_BUILD_STATIC must be ON")
endif()

# Dependencies
find_package(OpenSSL 1.0.0 REQUIRED)
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

# Library subdirectory
add_subdirectory(Source)

# Samples
if(MAFIANET_BUILD_SAMPLES)
    add_subdirectory(Samples)
    add_subdirectory(DependentExtensions)
endif()
