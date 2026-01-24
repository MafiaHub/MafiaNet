# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MafiaNet is a cross-platform network engine written in C++ for multiplayer games. It's a fork of SLikeNet, which itself continued RakNet. The project provides reliable UDP messaging, NAT traversal, peer-to-peer networking, and various game-specific networking features.

## Build Commands

```bash
# Configure (from repository root)
mkdir build && cd build
cmake ..

# Build
make                           # Unix/macOS
cmake --build . --config Release  # Cross-platform

# Build with samples enabled (Windows default, disabled on Unix)
cmake -DSLIKENET_ENABLE_SAMPLES=ON ..

# Build options
cmake -DSLIKENET_ENABLE_DLL=ON ..      # Build shared library (default: ON)
cmake -DSLIKENET_ENABLE_STATIC=ON ..   # Build static library (default: ON)
```

**Requirements:**
- CMake 3.15+
- C++11 compiler
- OpenSSL 1.0.0+
- Platform libs: ws2_32 (Windows), pthread (Unix)

## Running Tests

Tests are built as part of samples. After building with samples enabled:
```bash
./build/Samples/Tests/Tests
```

## Architecture

### Core Namespace
All types are in the `MafiaNet` namespace (e.g., `MafiaNet::RakPeerInterface`, `MafiaNet::BitStream`).

### Key Components

**Networking Core** (`Source/include/slikenet/`):
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
├── include/slikenet/   # Public headers (use these)
└── src/                # Implementation + crypto/

Lib/
├── LibStatic/          # Static library build
└── DLL/                # Shared library build

Samples/                # 80+ examples demonstrating features
DependentExtensions/    # Optional: MySQL, PostgreSQL, UPnP, Speex
```

### Basic Usage Pattern

```cpp
#include "slikenet/peerinterface.h"
#include "slikenet/BitStream.h"
#include "slikenet/MessageIdentifiers.h"

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
