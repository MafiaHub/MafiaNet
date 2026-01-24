#
# This file was taken from RakNet 4.082.
# Please see licenses/RakNet license.txt for the underlying license and related copyright.
#
#
# Modified work: Copyright (c) 2017-2019, SLikeSoft UG (haftungsbeschränkt)
#
# This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
# license found in the license.txt file in the root directory of this source tree.
#

cmake_minimum_required(VERSION 3.15)
GETCURRENTFOLDER()
FINDPORTAUDIO()
project(${current_folder})

IF(WIN32 AND NOT UNIX)
	FILE(GLOB ALL_CPP_SRCS *.cpp ${SLikeNet_SOURCE_DIR}/DependentExtensions/RakVoice.cpp)
	FILE(GLOB ALL_HEADER_SRCS *.h ${SLikeNet_SOURCE_DIR}/DependentExtensions/RakVoice.h)
	FILE(GLOB PORTAUDIOFILES ${portaudio_SOURCE_DIR}/pa_common/*.c ${portaudio_SOURCE_DIR}/pa_win_wmme/pa_win_wmme.c ${portaudio_SOURCE_DIR}/pa_common/*.h)
	FILE(GLOB SPEEXFILES ${speex_SOURCE_DIR}/win32/*.h ${speex_SOURCE_DIR}/include/*.h ${speex_SOURCE_DIR}/libspeex/*.h ${speex_SOURCE_DIR}/include/speex/*.h ${speex_SOURCE_DIR}/libspeex/*.c)
	LIST(REMOVE_ITEM SPEEXFILES 
	  ${speex_SOURCE_DIR}/libspeex/pcm_wrapper.h)
	LIST(REMOVE_ITEM SPEEXFILES 
	  ${speex_SOURCE_DIR}/libspeex/pcm_wrapper.c)
	SOURCE_GROUP(Speex FILES ${SPEEXFILES})
	SOURCE_GROUP(PortAudio FILES ${PORTAUDIOFILES})
	ADDCPPDEF(HAVE_CONFIG_H)
	include_directories(${SLIKENET_HEADER_FILES} ./ ${PORTAUDIO_INCLUDE_DIR} ${SLikeNet_SOURCE_DIR}/DependentExtensions ${speex_SOURCE_DIR}/include ${portaudio_SOURCE_DIR} ${speex_SOURCE_DIR}/win32) 
	add_executable(${current_folder} ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} ${PORTAUDIOFILES} ${SPEEXFILES})
	target_link_libraries(${current_folder} ${SLIKENET_COMMON_LIBS})
	VSUBFOLDER(${current_folder} Samples/Voice)
ELSE(WIN32 AND NOT UNIX)
	FILE(GLOB ALL_CPP_SRCS *.cpp)
	FILE(GLOB ALL_HEADER_SRCS *.h)
	include_directories(${SLIKENET_HEADER_FILES} ./ ${PORTAUDIO_INCLUDE_DIR} ${SLikeNet_SOURCE_DIR}/DependentExtensions) 
	add_executable(${current_folder} ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS})
	target_link_libraries(${current_folder} ${SLIKENET_COMMON_LIBS} ${PORTAUDIO_LIBRARIES} LibRakVoice)
ENDIF(WIN32 AND NOT UNIX)





