# MasterServer sample - Client and Server executables
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Shared sources for both client and server
set(MASTER_COMMON_SOURCES
    mastercommon.cpp
    MasterCommon.h
    MasterServerMessageIDs.h
)

# Master Server executable
mafianet_add_sample(
    NAME MasterServer
    FOLDER "MasterServer"
    SOURCES
        ${MASTER_COMMON_SOURCES}
        MasterServer.cpp
        MasterServer.h
        MasterServerMain.cpp
)

# Master Client executable
mafianet_add_sample(
    NAME MasterClient
    FOLDER "MasterServer"
    SOURCES
        ${MASTER_COMMON_SOURCES}
        MasterClient.cpp
        MasterClient.h
        MasterClientMain.cpp
)
