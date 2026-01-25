# AutopatcherServer requires PostgreSQL
find_package(PostgreSQL QUIET)

if(PostgreSQL_FOUND)
    # Build PostgreSQL repository
    add_library(AutopatcherPostgre STATIC
        ${PROJECT_SOURCE_DIR}/DependentExtensions/Autopatcher/AutopatcherPostgreRepository/AutopatcherPostgreRepository.cpp
        ${PROJECT_SOURCE_DIR}/DependentExtensions/Autopatcher/AutopatcherPostgreRepository/AutopatcherPostgreRepository.h
    )
    target_include_directories(AutopatcherPostgre PUBLIC
        ${PROJECT_SOURCE_DIR}/DependentExtensions/Autopatcher/AutopatcherPostgreRepository
        ${PROJECT_SOURCE_DIR}/DependentExtensions/Autopatcher
        ${PROJECT_SOURCE_DIR}/Source/include
        ${PostgreSQL_INCLUDE_DIRS}
    )
    target_link_libraries(AutopatcherPostgre PUBLIC
        MafiaNet::MafiaNetStatic
        Autopatcher
        ${PostgreSQL_LIBRARIES}
    )
    set_target_properties(AutopatcherPostgre PROPERTIES FOLDER "Extensions")

    mafianet_add_sample(NAME AutopatcherServer FOLDER "Autopatcher" LIBS Autopatcher AutopatcherPostgre)
    target_include_directories(AutopatcherServer PRIVATE
        ${PROJECT_SOURCE_DIR}/DependentExtensions/Autopatcher/AutopatcherPostgreRepository
    )
else()
    message(STATUS "PostgreSQL not found - skipping AutopatcherServer sample")
endif()
