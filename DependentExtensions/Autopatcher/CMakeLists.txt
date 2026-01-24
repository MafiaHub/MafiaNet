# Autopatcher Extension
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

file(GLOB AUTOPATCHER_SOURCES *.cpp)
file(GLOB AUTOPATCHER_HEADERS *.h)

add_library(Autopatcher STATIC
    ${AUTOPATCHER_SOURCES}
    ${AUTOPATCHER_HEADERS}
)

target_include_directories(Autopatcher PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/Source/include
)

target_link_libraries(Autopatcher PUBLIC
    MafiaNet::MafiaNetStatic
    BZip2
)

set_target_properties(Autopatcher PROPERTIES
    FOLDER "Extensions"
)
