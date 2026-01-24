# ChatExample - demonstrates basic chat functionality

add_executable(ChatServer "Server/Chat Example Server.cpp")
target_link_libraries(ChatServer PRIVATE MafiaNet::MafiaNetStatic)
target_include_directories(ChatServer PRIVATE ${PROJECT_SOURCE_DIR}/Source/include)
set_target_properties(ChatServer PROPERTIES FOLDER "Samples/Basic")

add_executable(ChatClient "Client/Chat Example Client.cpp")
target_link_libraries(ChatClient PRIVATE MafiaNet::MafiaNetStatic)
target_include_directories(ChatClient PRIVATE ${PROJECT_SOURCE_DIR}/Source/include)
set_target_properties(ChatClient PROPERTIES FOLDER "Samples/Basic")
