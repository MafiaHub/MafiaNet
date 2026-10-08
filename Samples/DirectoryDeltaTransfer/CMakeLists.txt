# DirectoryDeltaTransfer sample
# There are two test files with main() - build only directory_delta_transfer.cpp

mafianet_add_sample(
    NAME DirectoryDeltaTransfer
    FOLDER "FileTransfer"
    SOURCES directory_delta_transfer.cpp
)

# Optionally build the test version as a separate target
# mafianet_add_sample(
#     NAME DirectoryDeltaTransferTest
#     FOLDER "FileTransfer"
#     SOURCES DirectoryDeltaTransferTest.cpp
# )
