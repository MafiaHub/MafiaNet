# RakVoice Sample - Voice chat demonstration
#
# Requires PortAudio for audio I/O

# Check for PortAudio
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO portaudio-2.0)
endif()

# Build PortAudio from DependentExtensions if not found
if(NOT PORTAUDIO_FOUND)
    if(EXISTS "${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio")
        add_subdirectory(${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio portaudio)
        set(PORTAUDIO_LIBRARIES portaudio)
        set(PORTAUDIO_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio/include)
        set(PORTAUDIO_FOUND TRUE)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1")
        # Legacy PortAudio v18
        set(PORTAUDIO_SOURCES
            ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_common/pa_lib.c
            ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_common/pa_convert.c
        )
        if(WIN32)
            list(APPEND PORTAUDIO_SOURCES
                ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_win_wmme/pa_win_wmme.c
            )
        elseif(APPLE)
            list(APPEND PORTAUDIO_SOURCES
                ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_mac_core/pa_mac_core.c
            )
        else()
            list(APPEND PORTAUDIO_SOURCES
                ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_unix_oss/pa_unix_oss.c
                ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_unix_oss/pa_unix.c
            )
        endif()
        add_library(portaudio_v18 STATIC ${PORTAUDIO_SOURCES})
        target_include_directories(portaudio_v18 PUBLIC
            ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_common
        )
        if(WIN32)
            target_link_libraries(portaudio_v18 PRIVATE winmm)
        elseif(APPLE)
            find_library(COREAUDIO_LIBRARY CoreAudio)
            find_library(AUDIOUNIT_LIBRARY AudioUnit)
            find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox)
            target_link_libraries(portaudio_v18 PRIVATE
                ${COREAUDIO_LIBRARY}
                ${AUDIOUNIT_LIBRARY}
                ${AUDIOTOOLBOX_LIBRARY}
            )
        endif()
        set(PORTAUDIO_LIBRARIES portaudio_v18)
        set(PORTAUDIO_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/DependentExtensions/portaudio_v18_1/pa_common)
        set(PORTAUDIO_FOUND TRUE)
    endif()
endif()

if(PORTAUDIO_FOUND)
    add_executable(RakVoiceSample main.cpp)
    target_link_libraries(RakVoiceSample PRIVATE
        MafiaNet
        RakVoice
        ${PORTAUDIO_LIBRARIES}
    )
    target_include_directories(RakVoiceSample PRIVATE
        ${CMAKE_SOURCE_DIR}/DependentExtensions
        ${PORTAUDIO_INCLUDE_DIRS}
    )
    set_target_properties(RakVoiceSample PROPERTIES
        FOLDER "Samples/Voice"
        OUTPUT_NAME "RakVoice"
    )
else()
    message(STATUS "RakVoice sample disabled - PortAudio not found")
endif()
