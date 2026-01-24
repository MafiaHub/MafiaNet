# MafiaNet Dependent Extensions
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Always build bzip2 (required by Autopatcher)
add_subdirectory(bzip2-1.0.6)

# Autopatcher extension
add_subdirectory(Autopatcher)

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
