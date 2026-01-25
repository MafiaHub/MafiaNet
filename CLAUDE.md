# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MafiaNet is a cross-platform network engine written in C++ for multiplayer games. It's a fork of MafiaNet, which itself continued RakNet. The project provides reliable UDP messaging, NAT traversal, peer-to-peer networking, and various game-specific networking features.

## Build Commands

### Linux / macOS

```bash
# Configure (from repository root)
mkdir build && cd build
cmake ..

# Build
cmake --build .                       # All platforms
cmake --build . --config Release      # Release build

# Build with samples and tests
cmake -DMAFIANET_BUILD_SAMPLES=ON ..

# Build options
cmake -DMAFIANET_BUILD_SHARED=ON ..   # Build shared library (default: ON)
cmake -DMAFIANET_BUILD_STATIC=ON ..   # Build static library (default: ON)
cmake -DMAFIANET_BUILD_TESTS=ON ..    # Build test suite
```

### Windows (Visual Studio)

CMake generates Visual Studio solutions automatically. No pre-built `.sln` or `.vcxproj` files are included in the repository.

```powershell
# Generate Visual Studio 2022 solution
cmake -G "Visual Studio 17 2022" -A x64 -B build

# Generate Visual Studio 2019 solution
cmake -G "Visual Studio 16 2019" -A x64 -B build

# Build from command line
cmake --build build --config Release

# Or open build/MafiaNet.sln in Visual Studio
```

Available generators (run `cmake --help` for full list):
- `"Visual Studio 17 2022"` - VS 2022
- `"Visual Studio 16 2019"` - VS 2019
- `"Ninja"` - Fast builds with Ninja

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `MAFIANET_BUILD_SHARED` | ON | Build shared library (.dll/.so/.dylib) |
| `MAFIANET_BUILD_STATIC` | ON | Build static library (.lib/.a) |
| `MAFIANET_BUILD_SAMPLES` | OFF | Build sample applications |
| `MAFIANET_BUILD_TESTS` | OFF | Build test suite |

**Requirements:**
- CMake 3.21+
- C++17 compiler
- OpenSSL 1.0.0+
- Platform libs: ws2_32 (Windows), pthread (Unix)
- Internet connection (first configure fetches dependencies via FetchContent)

## Running Tests

Tests are built as part of samples. After building with samples enabled:
```bash
./build/Samples/Tests/Tests

# Run specific test by name
./build/Samples/Tests/Tests EightPeerTest
```

Available tests: `EightPeerTest`, `MaximumConnectTest`, `PeerConnectDisconnectWithCancelPendingTest`, `PeerConnectDisconnectTest`, `ManyClientsOneServerBlockingTest`, `ManyClientsOneServerNonBlockingTest`, `ManyClientsOneServerDeallocateBlockingTest`, `ReliableOrderedConvertedTest`, `DroppedConnectionConvertTest`, `ComprehensiveConvertTest`, `CrossConnectionConvertTest`, `PingTestsTest`, `OfflineMessagesConvertTest`, `LocalIsConnectedTest`, `SecurityFunctionsTest`, `ConnectWithSocketTest`, `SystemAddressAndGuidTest`, `PacketAndLowLevelTestsTest`, `MiscellaneousTestsTest`

## Architecture

### Namespaces
- Primary namespace: `MafiaNet` (e.g., `MafiaNet::RakPeerInterface`, `MafiaNet::BitStream`)
- Legacy alias: `RakNet` namespace is available for backward compatibility

### Key Components

**Networking Core** (`Source/include/mafianet/`):
- `RakPeerInterface` - Main entry point. Use `MafiaNet::RakPeerInterface::GetInstance()` to create
- `BitStream` - Binary serialization for packets
- `Packet` - Received network data container
- `SystemAddress` - Network endpoint identifier
- `MessageIdentifiers.h` - Packet type IDs (extend with `ID_USER_PACKET_ENUM`)

**Plugin System** (`PluginInterface2`):
- Extend functionality by attaching plugins to RakPeerInterface
- Examples: `FileListTransfer`, `FullyConnectedMesh2`, `NatPunchthroughClient`

**Data Structures** (`DS_*` headers):
- Custom containers: `DS_List`, `DS_Queue`, `DS_Map`, `DS_OrderedList`
- Memory pools: `DS_MemoryPool`

### Directory Structure

```
Source/
├── *.h                 # Thin wrapper headers (include these or mafianet/ versions)
├── include/mafianet/   # Full public API headers
└── src/                # Implementation files

Samples/                # 80+ examples demonstrating features
DependentExtensions/    # Optional integrations (database, voice, patching)
cmake/                  # CMake modules including FetchDependencies.cmake
```

### DependentExtensions

Optional integrations built when `MAFIANET_BUILD_SAMPLES=ON`:
- **Database**: `MySQLInterface`, `PostgreSQLInterface` - Database connectivity
- **Autopatcher**: Delta patching system for game updates (uses bzip2)
- **RakVoice**: Voice compression and transmission (uses Opus, RNNoise)
- **Lobby2**: Matchmaking and lobby system

Dependencies (bzip2, miniupnpc, Opus, RNNoise) are automatically fetched via CMake FetchContent at configure time.

### Basic Usage Pattern

```cpp
#include "mafianet/peerinterface.h"
#include "mafianet/BitStream.h"
#include "mafianet/MessageIdentifiers.h"

// Create peer
MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

// Start (server)
MafiaNet::SocketDescriptor sd(60000, 0);
peer->Startup(maxConnections, &sd, 1);
peer->SetMaximumIncomingConnections(maxConnections);

// Or connect (client)
peer->Connect("127.0.0.1", 60000, nullptr, 0);

// Process packets
MafiaNet::Packet* packet;
while ((packet = peer->Receive()) != nullptr) {
    switch (packet->data[0]) {
        case ID_NEW_INCOMING_CONNECTION:
            // Handle new connection
            break;
        // ... handle other message types
    }
    peer->DeallocatePacket(packet);
}

// Cleanup
MafiaNet::RakPeerInterface::DestroyInstance(peer);
```

## Key Samples

- `Samples/Ping/` - Basic UDP ping (beginner)
- `Samples/ChatExample/` - Simple chat application
- `Samples/ReplicaManager3/` - Object replication system
- `Samples/NATCompleteServer/` - NAT traversal demonstration
- `Samples/Tests/` - Comprehensive test suite
