# bzip2 library for MafiaNet
#
# Based on bzip2 1.0.6 by Julian Seward

set_source_files_properties(
    blocksort.c bzlib.c compress.c crctable.c
    decompress.c huffman.c randtable.c
    PROPERTIES LANGUAGE C
)

add_library(BZip2 STATIC
    blocksort.c
    bzlib.c
    compress.c
    crctable.c
    decompress.c
    huffman.c
    randtable.c
)

target_include_directories(BZip2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(BZip2 PROPERTIES
    FOLDER "Extensions"
)
