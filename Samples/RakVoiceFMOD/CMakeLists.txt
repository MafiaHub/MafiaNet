# RakVoice FMOD Sample - Voice chat with FMOD audio engine
#
# Requires FMOD library

# Try to find FMOD
find_path(FMOD_INCLUDE_DIR fmod.hpp
    HINTS
        "$ENV{FMOD_DIR}/api/core/inc"
        "$ENV{FMOD_DIR}/inc"
        "/usr/local/include/fmod"
        "/usr/include/fmod"
)

find_library(FMOD_LIBRARY
    NAMES fmod fmodex fmodL fmodexL
    HINTS
        "$ENV{FMOD_DIR}/api/core/lib"
        "$ENV{FMOD_DIR}/lib"
        "/usr/local/lib"
        "/usr/lib"
)

if(FMOD_INCLUDE_DIR AND FMOD_LIBRARY)
    add_executable(RakVoiceFMODSample
        main.cpp
        FMODVoiceAdapter.cpp
        FMODVoiceAdapter.h
    )

    target_include_directories(RakVoiceFMODSample PRIVATE
        ${CMAKE_SOURCE_DIR}/DependentExtensions
        ${FMOD_INCLUDE_DIR}
    )

    target_link_libraries(RakVoiceFMODSample PRIVATE
        MafiaNet
        RakVoice
        ${FMOD_LIBRARY}
    )

    set_target_properties(RakVoiceFMODSample PROPERTIES
        FOLDER "Samples/Voice"
        OUTPUT_NAME "RakVoiceFMOD"
    )
else()
    message(STATUS "RakVoice FMOD sample disabled - FMOD not found")
endif()
