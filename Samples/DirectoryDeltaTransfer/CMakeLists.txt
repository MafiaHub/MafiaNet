# DirectoryDeltaTransfer sample
# There are two test files with main() - build only DirectoryDeltaTransfer.cpp

mafianet_add_sample(
    NAME DirectoryDeltaTransfer
    FOLDER "FileTransfer"
    SOURCES DirectoryDeltaTransfer.cpp
)

# Optionally build the test version as a separate target
# mafianet_add_sample(
#     NAME DirectoryDeltaTransferTest
#     FOLDER "FileTransfer"
#     SOURCES DirectoryDeltaTransferTest.cpp
# )
