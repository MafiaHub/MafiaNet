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
GETCURRENTFOLDER()

IF(UNIX AND USEOIS AND NOT DISABLEDEPENDENCIES)
	project(${current_folder})
	FINDOGRE3D()
	FINDOIS()
	FILE(GLOB ALL_CPP_SRCS *.cpp)
	FILE(GLOB ALL_HEADER_SRCS *.h)
	FIXCOMPILEOPTIONS()
	include_directories(${SLIKENET_HEADER_FILES} ./ ${OGRE_INCLUDE_DIR} ${OIS_INCLUDE_DIR})
	add_executable(${current_folder} ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS})
	target_link_libraries(${current_folder} ${SLIKENET_COMMON_LIBS} ${OGRE_LIBRARIES} ${OIS_LIBRARIES})
	file(GLOB OGRE_RenderSystem_GL_LIBRARY_DBG_MOD  "/usr/lib/debug/usr/lib/OGRE/RenderSystem_GL.so")
	IF (NOT OGRE_RenderSystem_GL_LIBRARY_DBG_MOD)
		set(OGRE_RenderSystem_GL_LIBRARY_DBG_MOD ${OGRE_RenderSystem_GL_LIBRARY_DBG})
	ENDIF(NOT OGRE_RenderSystem_GL_LIBRARY_DBG_MOD)
	ADD_CUSTOM_COMMAND(TARGET ${current_folder} POST_BUILD COMMAND "cp" ARGS "-f" "${OGRE_RenderSystem_GL_LIBRARY_REL}" ./RenderSystem_GL.so)
	ADD_CUSTOM_COMMAND(TARGET ${current_folder} POST_BUILD COMMAND "cp" ARGS "-f" "${OGRE_RenderSystem_GL_LIBRARY_DBG_MOD}" ./RenderSystem_GL_D.so)
ENDIF(UNIX AND USEOIS AND NOT DISABLEDEPENDENCIES)



