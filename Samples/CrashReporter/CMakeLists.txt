#
# This file was taken from RakNet 4.082 without any modifications.
# Please see licenses/RakNet license.txt for the underlying license and related copyright.
#

cmake_minimum_required(VERSION 3.15)

IF (WIN32 AND NOT UNIX)
	GETCURRENTFOLDER()
	STANDARDSUBPROJECTWITHOPTIONS(${current_folder} "" "" "Dbghelp.lib")
ENDIF(WIN32 AND NOT UNIX)






