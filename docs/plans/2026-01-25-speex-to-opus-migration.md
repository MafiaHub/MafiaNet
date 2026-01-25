# Speex to Opus Migration Design

## Overview

Replace the deprecated Speex codec with Opus in RakVoice, adding RNNoise for noise suppression. This is a clean break with no backward compatibility to Speex-based clients.

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Noise suppression | RNNoise | Modern neural network approach, better quality than Speex preprocessor |
| Backward compatibility | Clean break | Simpler code, game deployments upgrade together |
| Sample rates | Native Opus (8k, 16k, 24k, 48k) | Embrace Opus capabilities, drop unsupported 32kHz |
| Dependencies | Bundled sources | Self-contained builds, works everywhere |

## Dependencies

**New (bundled in DependentExtensions/):**
- `opus-1.5.2/` - Opus codec (BSD license)
- `rnnoise/` - Mozilla RNNoise (BSD license)

**Removed:**
- `speex-1.1.12/` - entire directory
- `cmake/FindSpeex.cmake`
- `cmake/FindSpeexDSP.cmake`

## Public API Changes

### Init signature

```cpp
// Sample rates change from Speex (8k, 16k, 32k) to Opus (8k, 16k, 24k, 48k)
void Init(unsigned short sampleRate,    // 8000, 16000, 24000, 48000
          unsigned bufferSizeBytes);
```

### Removed methods

```cpp
void SetEncoderComplexity(int complexity);  // No Opus equivalent needed
int GetEncoderComplexity(void);
```

### Modified methods

```cpp
void SetVAD(bool enable);           // Maps to OPUS_SET_DTX
void SetVBR(bool enable);           // Maps to OPUS_SET_VBR
void SetNoiseFilter(bool enable);   // Toggles RNNoise processing
```

### New methods

```cpp
void SetSignalType(int type);  // OPUS_SIGNAL_VOICE (default) or OPUS_SIGNAL_MUSIC
```

## Internal Structures

### VoiceChannel

```cpp
struct VoiceChannel {
    RakNetGUID guid;

    // Opus state
    OpusEncoder *encoder;
    OpusDecoder *decoder;

    // RNNoise state
    DenoiseState *denoiser;

    // Frame configuration
    int frameSizeSamples;
    int maxPacketBytes;

    // Circular buffers (unchanged design)
    char *outgoingBuffer;
    char *incomingBuffer;
    unsigned outgoingReadIndex, outgoingWriteIndex;
    unsigned incomingReadIndex, incomingWriteIndex;

    // Packet sequencing (unchanged)
    unsigned short outgoingMessageNumber;
    unsigned short incomingMessageNumber;
};
```

### Frame sizes (20ms frames)

| Sample Rate | Frame Size (samples) | Buffer bytes (16-bit) |
|-------------|---------------------|----------------------|
| 8000 Hz     | 160                 | 320                  |
| 16000 Hz    | 320                 | 640                  |
| 24000 Hz    | 480                 | 960                  |
| 48000 Hz    | 960                 | 1920                 |

## Audio Pipeline

### Encoding (SendFrame to network)

```
User PCM audio (16-bit)
        |
        v
+-------------------+
| RNNoise denoise   | <- if noise filter enabled
| (float32 internal)|
+-------------------+
        |
        v
+-------------------+
| opus_encode()     | <- returns compressed packet
+-------------------+
        |
        v
Network (ID_RAKVOICE_DATA)
```

### Decoding (network to ReceiveFrame)

```
Network (ID_RAKVOICE_DATA)
        |
        v
+-------------------+
| opus_decode()     | <- handles packet loss via PLC
+-------------------+
        |
        v
Incoming circular buffer
        |
        v
ReceiveFrame() mixes all channels
```

### Key differences from Speex

- No `SpeexBits` bitstream - Opus returns raw packets directly
- RNNoise operates on float32, requires int16 conversion
- Opus has built-in Packet Loss Concealment (PLC)
- DTX replaces Speex VAD+preprocess combo

## CMake Configuration

### DependentExtensions/CMakeLists.txt

```cmake
# Opus library
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(opus-1.5.2)

# RNNoise library
add_subdirectory(rnnoise)

# RakVoice static library
add_library(RakVoice STATIC RakVoice.cpp RakVoice.h)
target_link_libraries(RakVoice PUBLIC MafiaNet PRIVATE opus rnnoise)
target_include_directories(RakVoice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

### DependentExtensions/rnnoise/CMakeLists.txt (new file)

```cmake
add_library(rnnoise STATIC
    src/denoise.c
    src/rnn.c
    src/rnn_data.c
    src/pitch.c
    src/kiss_fft.c
    src/celt_lpc.c
)
target_include_directories(rnnoise PUBLIC include)
target_compile_definitions(rnnoise PRIVATE COMPILE_OPUS=1)
```

## File Changes

### Delete

- `DependentExtensions/speex-1.1.12/` (entire directory)
- `cmake/FindSpeex.cmake`
- `cmake/FindSpeexDSP.cmake`

### Add

- `DependentExtensions/opus-1.5.2/` (download from opus-codec.org)
- `DependentExtensions/rnnoise/` (clone from github.com/xiph/rnnoise)
- `DependentExtensions/rnnoise/CMakeLists.txt`

### Modify

- `DependentExtensions/RakVoice.h` - API changes
- `DependentExtensions/RakVoice.cpp` - Full rewrite of internals
- `DependentExtensions/CMakeLists.txt` - Build configuration
- `Samples/RakVoice/CMakeLists.txt` - Simplified linking
- `Samples/RakVoice/main.cpp` - Update sample rates
- `Samples/RakVoiceDSound/CMakeLists.txt` - Same pattern
- `Samples/RakVoiceFMOD/CMakeLists.txt` - Same pattern

## Testing

1. **Build verification** - Compiles on Windows/macOS/Linux
2. **Basic sample test** - `Samples/RakVoice` loopback mode works
3. **Two-peer test** - Voice transmits between client/server
4. **Feature tests** - VAD/DTX, VBR, noise filter toggle all function
