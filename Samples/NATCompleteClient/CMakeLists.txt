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
SET(EXTRALIBS "") #Extra libraries, we have none
#note the .h files are included so they show up in the windows project
FILE(GLOB XML_FILES ${SLikeNet_SOURCE_DIR}/DependentExtensions/XML/*.cpp ${SLikeNet_SOURCE_DIR}/DependentExtensions/XML/*.h)#grab all files from xml folder and put them in the XML_FILES var
FILE(GLOB UPNP_FILES ${SLikeNet_SOURCE_DIR}/DependentExtensions/UPNP/*.cpp UPNP_FILES ${SLikeNet_SOURCE_DIR}/DependentExtensions/UPNP/*.h) #grab all files from UPNP folder and put them in the UPNP_FILES var
SET(EXTRASOURCES ${UPNP_FILES} ${XML_FILES}) #set the grabbed files as extra sources
SET(EXTRAINCLUDES ${SLikeNet_SOURCE_DIR}/DependentExtensions/XML  UPNP_FILES ${SLikeNet_SOURCE_DIR}/DependentExtensions/UPNP)#add the include directories
STANDARDSUBPROJECTWITHOPTIONSSET(${current_folder})
VSUBFOLDER(${current_folder} "Samples/NAT Punchthrough")





