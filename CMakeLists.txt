# This file was taken from RakNet 4.082.
# Please see licenses/RakNet license.txt for the underlying license and related copyright.
#
#
# Modified work: Copyright (c) 2016-2019, SLikeSoft UG (haftungsbeschränkt)
#
# This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
# license found in the license.txt file in the root directory of this source tree.
#

cmake_minimum_required(VERSION 3.15)

project(SLikeNet 
        VERSION 0.2.0
        LANGUAGES CXX)

# CMake policy settings
if(POLICY CMP0037)
    cmake_policy(SET CMP0037 NEW) # Target names should not be reserved
endif()
if(POLICY CMP0042)
    cmake_policy(SET CMP0042 NEW) # Use @rpath in target's install name
endif()

# Version numbers
set(SLikeNet_API_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-specific settings
if(WIN32)
    set(PROGRAMFILESX86 $ENV{PROGRAMFILES})
    string(REPLACE "\\" "/" PROGRAMFILESX86 "${PROGRAMFILESX86}")
endif()

# Find dependencies
find_package(OpenSSL 1.0.0 REQUIRED)

# Define library dependencies
set(SLIKENET_LIBRARY_LIBS 
    OpenSSL::Crypto 
    OpenSSL::SSL
)

if(WIN32)
    list(APPEND SLIKENET_LIBRARY_LIBS ws2_32.lib)
else()
    list(APPEND SLIKENET_LIBRARY_LIBS pthread)
endif()

# Options
option(SLIKENET_ENABLE_SAMPLES "Generate sample projects" ${WIN32})
option(SLIKENET_ENABLE_DLL "Generate shared library" ON)
option(SLIKENET_ENABLE_STATIC "Generate static library" ON)

# Header files location
set(SLIKENET_HEADER_FILES "${PROJECT_SOURCE_DIR}/Source")

# Include custom macros
include(./CmakeIncludes/CmakeMacros.txt)
FIXLINKOPTIONS()
FIXCOMPILEOPTIONS()

# Add library subdirectory
add_subdirectory(Lib)

set(SLIKENET_COMMON_LIBS SLikeNetLibStatic)

# Add samples if enabled
if(SLIKENET_ENABLE_SAMPLES)
    add_subdirectory(Samples)
endif()