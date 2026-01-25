# RNNoise - Neural Network Noise Suppression
# Bundled for MafiaNet RakVoice

cmake_minimum_required(VERSION 3.21)
project(rnnoise C)

set(RNNOISE_SOURCES
    src/denoise.c
    src/rnn.c
    src/nnet.c
    src/nnet_default.c
    src/rnnoise_tables.c
    src/pitch.c
    src/kiss_fft.c
    src/celt_lpc.c
)

add_library(rnnoise STATIC ${RNNOISE_SOURCES})

target_include_directories(rnnoise
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# RNNoise needs these defines - DO NOT define HAVE_CONFIG_H
target_compile_definitions(rnnoise PRIVATE
    COMPILE_OPUS=1
)

# Disable warnings for third-party code
if(MSVC)
    target_compile_options(rnnoise PRIVATE /W0)
else()
    target_compile_options(rnnoise PRIVATE -w)
endif()
