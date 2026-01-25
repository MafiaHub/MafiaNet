# MafiaNet CMake Helper Functions
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

include_guard(GLOBAL)

# mafianet_add_sample(<NAME> [FOLDER <folder>] [SOURCES <sources>] [LIBS <libs>] [INCLUDES <includes>])
#
# Creates a sample executable linked against MafiaNet.
# If SOURCES is not provided, globs *.cpp and *.h from the current directory.
function(mafianet_add_sample)
    cmake_parse_arguments(SAMPLE
        ""                          # Boolean args
        "NAME;FOLDER"               # Single-value args
        "SOURCES;LIBS;INCLUDES"     # Multi-value args
        ${ARGN}
    )

    if(NOT SAMPLE_NAME)
        message(FATAL_ERROR "mafianet_add_sample: NAME is required")
    endif()

    # Default: use all .cpp/.h in current directory
    if(NOT SAMPLE_SOURCES)
        file(GLOB SAMPLE_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        )
    endif()

    add_executable(${SAMPLE_NAME} ${SAMPLE_SOURCES})

    target_link_libraries(${SAMPLE_NAME} PRIVATE
        MafiaNet::MafiaNetStatic
        ${SAMPLE_LIBS}
    )

    target_include_directories(${SAMPLE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/Source              # Thin wrapper headers (RakString.h -> include/mafianet/string.h)
        ${PROJECT_SOURCE_DIR}/Source/include      # Modern headers (mafianet/...)
        ${PROJECT_SOURCE_DIR}/DependentExtensions
    )

    if(SAMPLE_INCLUDES)
        target_include_directories(${SAMPLE_NAME} PRIVATE ${SAMPLE_INCLUDES})
    endif()

    # IDE folder organization
    if(SAMPLE_FOLDER)
        set_target_properties(${SAMPLE_NAME} PROPERTIES
            FOLDER "Samples/${SAMPLE_FOLDER}"
        )
    else()
        set_target_properties(${SAMPLE_NAME} PROPERTIES
            FOLDER "Samples"
        )
    endif()
endfunction()

# mafianet_add_extension(<NAME> [SOURCES <sources>] [LIBS <libs>] [INCLUDES <includes>])
#
# Creates a static library extension for MafiaNet.
function(mafianet_add_extension)
    cmake_parse_arguments(EXT
        ""                          # Boolean args
        "NAME"                      # Single-value args
        "SOURCES;LIBS;INCLUDES"     # Multi-value args
        ${ARGN}
    )

    if(NOT EXT_NAME)
        message(FATAL_ERROR "mafianet_add_extension: NAME is required")
    endif()

    if(NOT EXT_SOURCES)
        file(GLOB EXT_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        )
    endif()

    add_library(${EXT_NAME} STATIC ${EXT_SOURCES})

    target_include_directories(${EXT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/Source/include
    )

    if(EXT_INCLUDES)
        target_include_directories(${EXT_NAME} PUBLIC ${EXT_INCLUDES})
    endif()

    if(EXT_LIBS)
        target_link_libraries(${EXT_NAME} PUBLIC ${EXT_LIBS})
    endif()

    set_target_properties(${EXT_NAME} PROPERTIES
        FOLDER "Extensions"
    )
endfunction()
