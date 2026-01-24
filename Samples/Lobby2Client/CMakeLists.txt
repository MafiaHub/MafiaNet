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
project(Lobby2Client)
include_directories(${SLIKENET_HEADER_FILES} ./ ${SLikeNet_SOURCE_DIR}/DependentExtensions/Lobby2 ${SLikeNet_SOURCE_DIR}/DependentExtensions/Lobby2/Rooms)
FILE(GLOB LOBBYFILES ${SLikeNet_SOURCE_DIR}/DependentExtensions/Lobby2/*.cpp ${SLikeNet_SOURCE_DIR}/DependentExtensions/Lobby2/*.h)
SOURCE_GROUP(Main FILES "Lobby2ClientSample.cpp")
add_executable(Lobby2Client "Lobby2ClientSample.cpp" ${LOBBYFILES})
target_link_libraries(Lobby2Client ${SLIKENET_COMMON_LIBS})
IF(WIN32 AND NOT UNIX)
	VSUBFOLDER(Lobby2Client "Samples/Lobby2")
ENDIF(WIN32 AND NOT UNIX)






