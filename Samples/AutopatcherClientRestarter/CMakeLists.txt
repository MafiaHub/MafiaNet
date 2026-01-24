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
project(AutopatcherClientRestarter)
include_directories(${SLIKENET_HEADER_FILES} ./) 
add_executable(AutopatcherClientRestarter "main.cpp")
target_link_libraries(AutopatcherClientRestarter ${SLIKENET_COMMON_LIBS})
VSUBFOLDER(AutopatcherClientRestarter "Samples/AutoPatcher/Client")





