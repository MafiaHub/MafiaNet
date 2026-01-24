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
project(AutopatcherServer)

IF(WIN32 AND NOT UNIX)
	FILE(GLOB AUTOSRC "${Autopatcher_SOURCE_DIR}/AutopatcherServer.cpp" "${Autopatcher_SOURCE_DIR}/MemoryCompressor.cpp" "${Autopatcher_SOURCE_DIR}/CreatePatch.cpp" "${Autopatcher_SOURCE_DIR}/AutopatcherServer.h")
	FILE(GLOB BZSRC "${BZip2_SOURCE_DIR}/*.c" "${BZip2_SOURCE_DIR}/*.h")
	LIST(REMOVE_ITEM BZSRC "${BZip2_SOURCE_DIR}/dlltest.c" "${BZip2_SOURCE_DIR}/mk251.c" "${BZip2_SOURCE_DIR}/bzip2recover.c")
	SOURCE_GROUP(BZip FILES ${BZSRC})
	SOURCE_GROUP("Source Files" FILES ${AUTOSRC})
	SOURCE_GROUP(Main "AutopatcherServerTest.cpp")
	include_directories(${SLIKENET_HEADER_FILES} ./ ${AutopatcherPostgreRepository_SOURCE_DIR} ${PostgreSQLInterface_SOURCE_DIR} ${Autopatcher_SOURCE_DIR} ${BZip2_SOURCE_DIR}) 
	add_executable(AutopatcherServer_PostgreSQL "AutopatcherServerTest.cpp" ${AUTOSRC} ${BZSRC})
	target_link_libraries(AutopatcherServer_PostgreSQL ${SLIKENET_COMMON_LIBS} AutopatcherPostgreRepository)
	VSUBFOLDER(AutopatcherServer_PostgreSQL "Samples/AutoPatcher/Server/PostgreSQL")
ELSE(WIN32 AND NOT UNIX)
	include_directories(${SLIKENET_HEADER_FILES} ./ ${AutopatcherPostgreRepository_SOURCE_DIR} ${PostgreSQLInterface_SOURCE_DIR} ${Autopatcher_SOURCE_DIR}) 
	add_executable(AutopatcherServer_PostgreSQL "AutopatcherServerTest.cpp")
	target_link_libraries(AutopatcherServer_PostgreSQL ${SLIKENET_COMMON_LIBS} LibAutopatcher AutopatcherPostgreRepository LibPostgreSQLInterface)
ENDIF(WIN32 AND NOT UNIX)







