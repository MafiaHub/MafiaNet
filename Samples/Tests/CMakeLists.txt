mafianet_add_sample(NAME Tests FOLDER "Tests")

# CryptoUnitTest calls sodium functions directly (not just via the core lib),
# so the Tests target needs sodium headers + lib explicitly.
target_link_libraries(Tests PRIVATE sodium)
