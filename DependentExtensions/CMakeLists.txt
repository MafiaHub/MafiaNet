# MafiaNet Dependent Extensions
#
# Dependencies (bzip2, miniupnpc, opus, rnnoise) are fetched via FetchContent
# in cmake/FetchDependencies.cmake
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Autopatcher extension (uses bzip2 from FetchContent)
add_subdirectory(Autopatcher)

# Lobby2 extension (matchmaking and lobby system)
add_subdirectory(Lobby2)

# RakVoice - voice compression and transmission (uses opus, rnnoise from FetchContent)
add_library(RakVoice STATIC RakVoice.cpp RakVoice.h)
target_link_libraries(RakVoice PUBLIC MafiaNet PRIVATE opus rnnoise)
target_include_directories(RakVoice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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
