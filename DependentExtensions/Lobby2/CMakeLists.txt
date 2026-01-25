# Lobby2 Extension
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

file(GLOB LOBBY2_SOURCES *.cpp)
file(GLOB LOBBY2_HEADERS *.h)

file(GLOB ROOMS_SOURCES Rooms/*.cpp)
file(GLOB ROOMS_HEADERS Rooms/*.h)

add_library(Lobby2 STATIC
    ${LOBBY2_SOURCES}
    ${LOBBY2_HEADERS}
    ${ROOMS_SOURCES}
    ${ROOMS_HEADERS}
)

target_include_directories(Lobby2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Rooms
    ${PROJECT_SOURCE_DIR}/Source/include
)

target_link_libraries(Lobby2 PUBLIC
    MafiaNet::MafiaNetStatic
)

set_target_properties(Lobby2 PROPERTIES
    FOLDER "Extensions"
)
