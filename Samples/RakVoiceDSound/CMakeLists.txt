# RakVoice DirectSound Sample - Voice chat with DirectSound
#
# Windows only - requires DirectSound SDK

if(NOT WIN32)
    return()
endif()

# Check for DirectSound
find_path(DSOUND_INCLUDE_DIR dsound.h
    HINTS
        "$ENV{DXSDK_DIR}/Include"
        "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Include"
)

find_library(DSOUND_LIBRARY dsound
    HINTS
        "$ENV{DXSDK_DIR}/Lib/x64"
        "$ENV{DXSDK_DIR}/Lib/x86"
        "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Lib/x64"
        "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Lib/x86"
)

find_library(DXERR_LIBRARY dxerr
    HINTS
        "$ENV{DXSDK_DIR}/Lib/x64"
        "$ENV{DXSDK_DIR}/Lib/x86"
        "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Lib/x64"
        "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Lib/x86"
)

if(DSOUND_INCLUDE_DIR AND DSOUND_LIBRARY)
    add_executable(RakVoiceDSoundSample
        main.cpp
        DSoundVoiceAdapter.cpp
        DSoundVoiceAdapter.h
    )

    target_include_directories(RakVoiceDSoundSample PRIVATE
        ${CMAKE_SOURCE_DIR}/DependentExtensions
        ${DSOUND_INCLUDE_DIR}
    )

    target_link_libraries(RakVoiceDSoundSample PRIVATE
        MafiaNet
        RakVoice
        ${DSOUND_LIBRARY}
        ${DXERR_LIBRARY}
        winmm
    )

    target_compile_definitions(RakVoiceDSoundSample PRIVATE
        _UNICODE
        UNICODE
    )

    set_target_properties(RakVoiceDSoundSample PROPERTIES
        FOLDER "Samples/Voice"
        OUTPUT_NAME "RakVoiceDSound"
    )
else()
    message(STATUS "RakVoice DirectSound sample disabled - DirectSound SDK not found")
endif()
