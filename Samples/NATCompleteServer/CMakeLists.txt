mafianet_add_sample(
    NAME NATCompleteServer
    FOLDER "NAT"
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${PROJECT_SOURCE_DIR}/Samples/CloudServer/CloudServerHelper.cpp
        ${PROJECT_SOURCE_DIR}/Samples/CloudServer/CloudServerHelper.h
    INCLUDES
        ${PROJECT_SOURCE_DIR}/Samples/CloudServer
)
