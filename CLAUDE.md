# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MafiaNet is a cross-platform network engine written in C++ for multiplayer games. It's a fork of SLikeNet, which itself continued RakNet. The project provides reliable UDP messaging, NAT traversal, peer-to-peer networking, and various game-specific networking features.

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

# Quick rebuild after changes (from repository root)
cmake --build build -j$(nproc)      # Linux
cmake --build build -j$(sysctl -n hw.ncpu)  # macOS

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
- OpenSSL 3.0+
- Platform libs: ws2_32 (Windows), pthread (Unix)
- Internet connection (first configure fetches dependencies via FetchContent)

## Running Tests

All tests are GoogleTest, built with `MAFIANET_BUILD_TESTS=ON` (requires `MAFIANET_BUILD_STATIC=ON`) and driven by CTest. Two binaries under `Tests/`:

- **`UnitTests`** (`Tests/Unit/`, label `unit`): hermetic and deterministic — no loopback networking, no wall-clock timing. A started-but-unconnected peer with synchronous assertions is acceptable. New tests for pure logic go here.
- **`IntegrationTests`** (`Tests/Integration/`, label `integration`): real UDP over loopback. Discovered tests get `RUN_SERIAL` (several bind fixed ports) and a 600s timeout. Shared polling/connect helpers live in `Tests/Support/`.

Under ctest, every `TEST()` runs in its own process — never write tests that depend on state from a previous `TEST()`. Long sequential networking scenarios belong in a single `TEST_F`.

```bash
cmake -B build -DMAFIANET_BUILD_TESTS=ON
cmake --build build

ctest --test-dir build --output-on-failure          # everything
ctest --test-dir build -L unit                      # hermetic unit suite only
ctest --test-dir build -L integration               # loopback integration suite only
ctest --test-dir build -R "DispatcherLive"          # by name pattern
```

For debugging, run a binary directly with a filter: `./build/Tests/IntegrationTests --gtest_filter='DispatcherLive.*'`; reproduce flakes with `--gtest_repeat=100 --gtest_break_on_failure`.

Integration tests must prefer OS-assigned ephemeral ports (`SocketDescriptor(0, "127.0.0.1")` + `GetInternalID().GetPort()`) over fixed ports, and must poll for conditions with a deadline rather than assert immediately after a state change (packets surface asynchronously from the network thread). Peers must be cleaned up in fixture `TearDown()` (or RAII `Peer` handles) so a failed `ASSERT_` doesn't leak them. CI retries transient integration failures via `ctest --repeat until-pass:3`; content/correctness assertions should be written so a real regression fails deterministically on every attempt.

## Architecture

### Namespaces
- Primary namespace: `MafiaNet` (e.g., `MafiaNet::RakPeerInterface`, `MafiaNet::BitStream`) — used exclusively throughout the library
- Short-hand alias: the `MNet` preprocessor macro (defined in `mafianet/defines.h`) expands to `MafiaNet` as a convenience shorthand

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
├── include/mafianet/   # Public API headers (include as "mafianet/...")
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
- `Tests/` - GoogleTest suites (`Unit/`, `Integration/`, shared helpers in `Support/`)

## Releasing (Version Bump Procedure)

MafiaNet uses [Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`).
The version string lives in **several** places that must all be kept in sync. When
cutting a release, bump **every** location below to the new version:

| File | What to change |
|------|----------------|
| `CMakeLists.txt` | `project(MafiaNet VERSION X.Y.Z ...)` — the canonical source of truth |
| `Source/include/mafianet/version.h` | `MAFIANET_VERSION`, `MAFIANET_VERSION_NUMBER_INT`, and the `MAJOR`/`MINOR`/`PATCH` defines (leave the deprecated `RAKNET_*` / `SLIKENET_*` defines untouched) |
| `docs/conf.py` | `version` and `release` |
| `docs/Doxyfile` | `PROJECT_NUMBER` |
| `docs/changelog.rst` | Add a new `Version X.Y.Z` section at the top |
| `README.md` | Add a new `### Version X.Y.Z (Latest)` entry under Changelog and drop `(Latest)` from the previous one |

### Steps

1. Determine the previous release boundary (e.g. `git log --reverse <prev-tag>..HEAD`,
   or the last `Version` entry in `docs/changelog.rst` if no tag exists).
2. Summarize the changes since then from the commit history and write changelog
   entries in `docs/changelog.rst` and `README.md`.
3. Bump the version in **all** files in the table above.
4. Commit: `git commit -am "chore(release): vX.Y.Z"`.
5. Tag: `git tag -a vX.Y.Z -m "MafiaNet vX.Y.Z"`.
6. Push: `git push origin <branch> --follow-tags`.
7. Create the GitHub release: `gh release create vX.Y.Z --title "vX.Y.Z" --notes "..."`.

> Note: releases are tagged `vX.Y.Z` (with the `v` prefix); the in-code/CMake
> version strings are `X.Y.Z` (no prefix).
