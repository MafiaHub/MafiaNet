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
project("Chat Example")
include_directories(${SLIKENET_HEADER_FILES} ./) 
add_executable(ChatServer "Server/Chat Example Server.cpp")
add_executable(ChatClient "Client/Chat Example Client.cpp")

target_link_libraries(ChatServer ${SLIKENET_COMMON_LIBS})
VSUBFOLDER(ChatServer "Samples")

target_link_libraries(ChatClient ${SLIKENET_COMMON_LIBS})
VSUBFOLDER(ChatClient "Samples")






