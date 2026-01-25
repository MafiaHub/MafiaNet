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
FINDIRRLICHT()
IF(WIN32 AND NOT UNIX)
	project(${current_folder})
	FILE(GLOB ALL_CPP_SRCS *.cpp "${MafiaNet_SOURCE_DIR}/Samples/PHPDirectoryServer2/PHPDirectoryServer2.cpp")
	FILE(GLOB ALL_HEADER_SRCS *.h "${MafiaNet_SOURCE_DIR}/Samples/PHPDirectoryServer2/PHPDirectoryServer2.h")
	FIXCOMPILEOPTIONS()
	include_directories(${SLIKENET_HEADER_FILES} ./ ${IRRLICHT_INCLUDE_DIR}) 
	add_executable(${current_folder} WIN32 ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS})
	target_link_libraries(${current_folder} ${SLIKENET_COMMON_LIBS})
	set_target_properties(${current_folder} PROPERTIES PROJECT_GROUP "Samples/3D Demos")
ELSE(WIN32 AND NOT UNIX)
	project(${current_folder})
	FINDIRRKLANG()
	FILE(GLOB ALL_CPP_SRCS *.cpp "${MafiaNet_SOURCE_DIR}/Samples/PHPDirectoryServer2/PHPDirectoryServer2.cpp")
	FILE(GLOB ALL_HEADER_SRCS *.h "${MafiaNet_SOURCE_DIR}/Samples/PHPDirectoryServer2/PHPDirectoryServer2.h")
	FIXCOMPILEOPTIONS()
	include_directories(${SLIKENET_HEADER_FILES} ./ ${IRRLICHT_INCLUDE_DIR} ${IRRKLANG_INCLUDE_DIR}) 
	add_executable(${current_folder} WIN32 ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS})
	target_link_libraries(${current_folder} ${SLIKENET_COMMON_LIBS} ${IRRLICHT_LIBRARIES} ${IRRKLANG_LIBRARIES})
ENDIF(WIN32 AND NOT UNIX)






