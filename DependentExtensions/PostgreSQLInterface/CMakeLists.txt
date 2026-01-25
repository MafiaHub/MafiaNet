# PostgreSQL Interface Extension
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license
#
# Note: PostgreSQL is found by the parent CMakeLists.txt before including this

file(GLOB POSTGRE_SOURCES *.cpp)
file(GLOB POSTGRE_HEADERS *.h)

add_library(PostgreSQLInterface STATIC
    ${POSTGRE_SOURCES}
    ${POSTGRE_HEADERS}
)

target_include_directories(PostgreSQLInterface PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/Source/include
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(PostgreSQLInterface PUBLIC
    MafiaNet::MafiaNetStatic
    ${PostgreSQL_LIBRARIES}
)

set_target_properties(PostgreSQLInterface PROPERTIES
    FOLDER "Extensions"
)
