# MySQL Interface Extension
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license
#
# Note: MySQL is found by the parent CMakeLists.txt before including this

file(GLOB MYSQL_SOURCES *.cpp)
file(GLOB MYSQL_HEADERS *.h)

add_library(MySQLInterface STATIC
    ${MYSQL_SOURCES}
    ${MYSQL_HEADERS}
)

target_include_directories(MySQLInterface PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/Source/include
    ${MYSQL_INCLUDE_DIR}
)

target_link_libraries(MySQLInterface PUBLIC
    MafiaNet::MafiaNetStatic
    ${MYSQL_LIBRARIES}
)

set_target_properties(MySQLInterface PROPERTIES
    FOLDER "Extensions"
)
