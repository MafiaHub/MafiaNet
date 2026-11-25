mafianet_add_sample(NAME PlayerLogin FOLDER "Players" SOURCES main.cpp protocol.h)
mafianet_add_sample(NAME PlayerLoginClient FOLDER "Players" SOURCES client.cpp protocol.h)
mafianet_enable_warnings(PlayerLogin PlayerLoginClient)
