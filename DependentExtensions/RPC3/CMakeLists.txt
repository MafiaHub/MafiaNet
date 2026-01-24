#
# This file was taken from RakNet 4.082.
# Please see licenses/RakNet license.txt for the underlying license and related copyright.
#
#
# Modified work: Copyright (c) 2019, SLikeSoft UG (haftungsbeschränkt)
#
# This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
# license found in the license.txt file in the root directory of this source tree.
#

cmake_minimum_required(VERSION 3.15)
project(LibRPC3)
IF (NOT WIN32 AND UNIX)
	FILE(GLOB ALL_HEADER_SRCS *.h)
	FILE(GLOB ALL_CPP_SRCS *.cpp)
	FINDBOOST()
	include_directories("${Boost_INCLUDE_DIRS}" ${SLIKENET_HEADER_FILES}) 
	add_library(LibRPC3 STATIC ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} "${Boost_INCLUDE_DIRS}" "${Boost_INCLUDE_DIRS}/boost")
	target_link_libraries(LibRPC3 ${SLIKENET_COMMON_LIBS} ${Boost_LIBRARIES})
ENDIF(NOT WIN32 AND UNIX)



