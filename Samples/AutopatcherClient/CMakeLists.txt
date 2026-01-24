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
project(AutopatcherClient)

set(Autopatcher_SOURCE_DIR ${SLikeNet_SOURCE_DIR}/DependentExtensions/Autopatcher)
set(BZip2_SOURCE_DIR ${SLikeNet_SOURCE_DIR}/DependentExtensions/bzip2-1.0.6)

include_directories(${SLIKENET_HEADER_FILES} ./ ${Autopatcher_SOURCE_DIR} ${BZip2_SOURCE_DIR} )
FILE(GLOB AUTOSRC "${Autopatcher_SOURCE_DIR}/*.cpp" "${Autopatcher_SOURCE_DIR}/*.h")
LIST(REMOVE_ITEM AUTOSRC "${Autopatcher_SOURCE_DIR}/AutopatcherServer.cpp" "${Autopatcher_SOURCE_DIR}/AutopatcherServer.h" )
FILE(GLOB BZSRC "${BZip2_SOURCE_DIR}/*.c" "${BZip2_SOURCE_DIR}/*.h")
LIST(REMOVE_ITEM BZSRC "${BZip2_SOURCE_DIR}/dlltest.c" "${BZip2_SOURCE_DIR}/mk251.c" "${BZip2_SOURCE_DIR}/bzip2recover.c")
SOURCE_GROUP(BZip2 FILES ${BZSRC})
SET(WRAPFILES "${Autopatcher_SOURCE_DIR}/MemoryCompressor.cpp" "${Autopatcher_SOURCE_DIR}/MemoryCompressor.h")
LIST(REMOVE_ITEM AUTOSRC ${WRAPFILES})
SOURCE_GROUP(Client_Files FILES ${AUTOSRC})
SOURCE_GROUP(MAIN FILES "AutopatcherClientTest.cpp")
SOURCE_GROUP(BZip2Wrapper FILES ${WRAPFILES})
add_executable(AutopatcherClient "AutopatcherClientTest.cpp" ${AUTOSRC} ${BZSRC} ${WRAPFILES})
target_link_libraries(AutopatcherClient ${SLIKENET_COMMON_LIBS})

##VSUBFOLDER(AutopatcherClient "Samples/AutoPatcher/Client")


