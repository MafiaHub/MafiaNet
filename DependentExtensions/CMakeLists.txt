# MafiaNet Dependent Extensions
#
# Dependencies (bzip2, miniupnpc, jansson) are fetched via FetchContent
# in cmake/FetchDependencies.cmake
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Autopatcher extension (uses bzip2 from FetchContent)
add_subdirectory(Autopatcher)

# Lobby2 extension (matchmaking and lobby system)
add_subdirectory(Lobby2)

# RakVoice (voice compression using Opus + RNNoise) is built into the core
# MafiaNet library; see Source/CMakeLists.txt.

# Optional extensions based on available dependencies

# MySQL interface
find_package(MySQL QUIET)
if(MySQL_FOUND)
    add_subdirectory(MySQLInterface)
endif()

# PostgreSQL interface
find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    add_subdirectory(PostgreSQLInterface)
endif()

# RPC3 requires Boost
find_package(Boost QUIET)
if(Boost_FOUND)
    add_subdirectory(RPC3)
endif()
