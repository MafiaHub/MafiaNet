#
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

project(SLikeNetLibStatic VERSION 0.2.0 LANGUAGES CXX)

# Source files
file(GLOB ALL_HEADER_SRCS ${SLikeNet_SOURCE_DIR}/Source/include/slikenet/*.h)
file(GLOB ALL_COMPATIBILITY_HEADER_SRC ${SLikeNet_SOURCE_DIR}/Source/*.h)
file(GLOB ALL_COMPATIBILITY_HEADER_SRC_2 ${SLikeNet_SOURCE_DIR}/Source/slikenet/*.h)
file(GLOB ALL_CPP_SRCS ${SLikeNet_SOURCE_DIR}/Source/src/*.cpp)
file(GLOB CRYPTO_CPP_SRCS ${SLikeNet_SOURCE_DIR}/Source/src/crypto/*.cpp)

# Source groups for Visual Studio organization
source_group("Header Files" FILES ${Header_Files})
source_group("Header Files\\crypto" FILES ${Header_Files__crypto})
source_group("Source Files" FILES ${Source_Files})
source_group("Source Files\\crypto" FILES ${Source_Files__crypto})

# Create static library
add_library(${PROJECT_NAME} STATIC 
    ${ALL_CPP_SRCS} 
    ${CRYPTO_CPP_SRCS} 
    ${ALL_HEADER_SRCS}
    ${Header_Files}
    ${Header_Files__crypto}
    ${Source_Files}
    ${Source_Files__crypto}
)

# Set target properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VS_GLOBAL_KEYWORD "Win32Proj"
)

# Include directories
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Source/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../DependentExtensions>
    $<$<PLATFORM_ID:Windows>:$<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:${CMAKE_CURRENT_SOURCE_DIR}/../../DependentExtensions/openssl/include/x86>>
    $<$<PLATFORM_ID:Windows>:$<$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>:${CMAKE_CURRENT_SOURCE_DIR}/../../DependentExtensions/openssl/include/x64>>
)

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:_RELEASE>
    $<$<CONFIG:RetailUnicode>:_RETAIL UNICODE _UNICODE>
    $<$<CONFIG:Retail>:_RETAIL>
    $<$<PLATFORM_ID:Windows>:WIN32 _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_DEPRECATE _RAKNET_LIB>
)

# Set compiler options
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4 
        /WX 
        /Gm
        /Y-
        $<$<CONFIG:Debug>:/Od /sdl /RTC1>
        $<$<CONFIG:Release>:/sdl>
        $<$<CONFIG:Retail>:/GS->
    )
    
    # Set MSVC runtime library
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY_DEBUG "MultiThreadedDebug"
        MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )

    # Link options for MSVC
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            STATIC_LIBRARY_FLAGS "/NODEFAULTLIB:\"LIBCD.lib LIBCMTD.lib MSVCRT.lib\""
        )
    endif()
endif()

# Output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Lib"
)

# Installation
if(NOT WIN32)
    install(TARGETS ${PROJECT_NAME}
        EXPORT ${PROJECT_NAME}
        DESTINATION lib/slikenet-${PROJECT_VERSION}
    )
    install(FILES 
        ${ALL_COMPATIBILITY_HEADER_SRC} 
        DESTINATION include/slikenet-${PROJECT_VERSION}
    )
    install(FILES 
        ${ALL_COMPATIBILITY_HEADER_SRC_2} 
        DESTINATION include/slikenet-${PROJECT_VERSION}/slikenet
    )
    install(FILES 
        ${ALL_HEADER_SRCS} 
        DESTINATION include/slikenet-${PROJECT_VERSION}/include/slikenet
    )
    install(FILES 
        ../../slikenet-config.cmake 
        ${CMAKE_CURRENT_BINARY_DIR}/slikenet-config-version.cmake 
        DESTINATION lib/slikenet-${PROJECT_VERSION}
    )
    install(EXPORT ${PROJECT_NAME}
        FILE slikenet.cmake
        DESTINATION lib/slikenet-${PROJECT_VERSION}
    )
endif()