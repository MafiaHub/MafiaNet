# RPC3 Extension
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license
#
# Requires Boost

file(GLOB RPC3_SOURCES *.cpp)
file(GLOB RPC3_HEADERS *.h)

add_library(RPC3Lib STATIC
    ${RPC3_SOURCES}
    ${RPC3_HEADERS}
)
add_library(MafiaNet::RPC3 ALIAS RPC3Lib)

target_include_directories(RPC3Lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/Source/include
)

target_link_libraries(RPC3Lib PUBLIC
    MafiaNet::MafiaNetStatic
    Boost::boost
)

set_target_properties(RPC3Lib PROPERTIES
    FOLDER "Extensions"
)
