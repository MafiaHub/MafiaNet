<div align="center">
  <a href="https://github.com/MafiaHub/Framework">
    <img src="https://github.com/MafiaHub/Framework/assets/9026786/43e839f2-f207-47bf-aa59-72371e8403ba" alt="MafiaHub" />
  </a>

  <h1>MafiaNet</h1>

  <p><strong>A modern, cross-platform networking engine for multiplayer games</strong></p>

  <p>
    <a href="https://discord.gg/eBQ4QHX"><img src="https://img.shields.io/discord/402098213114347520.svg?label=Discord&logo=discord&logoColor=white" alt="Discord" /></a>
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License" /></a>
    <img src="https://img.shields.io/badge/C%2B%2B-11-blue.svg?logo=cplusplus" alt="C++11" />
    <img src="https://img.shields.io/badge/CMake-3.15+-064F8C.svg?logo=cmake" alt="CMake" />
  </p>

  <p>
    <a href="#quick-start">Quick Start</a> •
    <a href="#features">Features</a> •
    <a href="#documentation">Documentation</a> •
    <a href="#community">Community</a>
  </p>
</div>

---

## About

MafiaNet is an actively maintained networking library built for game developers who need reliable, high-performance multiplayer networking. Built on the foundation of RakNet and SLikeNet, MafiaNet delivers battle-tested networking capabilities with modern C++ standards and security practices.

**Supported Platforms:** Windows, Linux, macOS (primary) | iOS, Android (limited)

## Quick Start

### Requirements

- CMake 3.15+
- C++11 compatible compiler
- OpenSSL 1.0.0+

### Building

```bash
git clone https://github.com/MafiaHub/MafiaNet.git
cd MafiaNet
mkdir build && cd build
cmake ..
cmake --build .
```

### Basic Usage

```cpp
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"

// Create a peer
MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

// Start as server
MafiaNet::SocketDescriptor sd(60000, 0);
peer->Startup(32, &sd, 1);
peer->SetMaximumIncomingConnections(32);

// Or connect as client
peer->Connect("127.0.0.1", 60000, nullptr, 0);

// Process incoming packets
MafiaNet::Packet* packet;
while ((packet = peer->Receive()) != nullptr) {
    switch (packet->data[0]) {
        case ID_NEW_INCOMING_CONNECTION:
            printf("Client connected\n");
            break;
        case ID_CONNECTION_REQUEST_ACCEPTED:
            printf("Connected to server\n");
            break;
    }
    peer->DeallocatePacket(packet);
}

// Cleanup
MafiaNet::RakPeerInterface::DestroyInstance(peer);
```

## Features

<table>
<tr>
<td width="50%" valign="top">

### Core Networking
- Reliable UDP with automatic retransmission
- Packet ordering and sequencing
- Automatic packet splitting for large messages
- IPv4 and IPv6 dual-stack support
- SSL/TLS encryption via OpenSSL
- Connection statistics and monitoring

</td>
<td width="50%" valign="top">

### Multiplayer Systems
- Peer-to-peer and client-server architectures
- NAT punchthrough and traversal
- Host migration
- Room and lobby management
- Object replication (ReplicaManager3)
- Remote procedure calls (RPC3/RPC4)

</td>
</tr>
<tr>
<td width="50%" valign="top">

### Development Tools
- Packet logger with multiple outputs
- Network simulator (latency, packet loss)
- Bandwidth monitoring and limiting
- Message filtering
- Comprehensive debug statistics

</td>
<td width="50%" valign="top">

### Additional Features
- File transfer system
- Delta patching (Autopatcher)
- LAN server discovery
- Database integration (MySQL, PostgreSQL, SQLite)
- Plugin architecture
- UPnP support

</td>
</tr>
</table>

## Documentation

| Resource | Description |
|----------|-------------|
| [Samples](./Samples) | 80+ example projects covering all major features |
| [API Headers](./Source/include/slikenet) | Well-documented public API |
| [CLAUDE.md](./CLAUDE.md) | Quick reference for AI-assisted development |

### Key Samples

| Sample | Description |
|--------|-------------|
| [Ping](./Samples/Ping) | Basic UDP communication |
| [ChatExample](./Samples/ChatExample) | Simple chat application |
| [ReplicaManager3](./Samples/ReplicaManager3) | Object replication system |
| [NATCompleteClient/Server](./Samples/NATCompleteClient) | NAT traversal demonstration |
| [FileListTransfer](./Samples/FileListTransfer) | File transfer system |

## Project Structure

```
MafiaNet/
├── Source/
│   ├── include/slikenet/   # Public API headers
│   └── src/                # Implementation
├── Lib/                    # Library build outputs
├── Samples/                # Example projects
└── DependentExtensions/    # Optional integrations
```

## Roadmap

- [ ] Remove legacy/deprecated code
- [ ] Modernize CMake build system
- [ ] Add GitHub Actions CI/CD
- [ ] Improve documentation
- [ ] Add unit test coverage
- [ ] Create versioned releases

## Background

MafiaNet continues the legacy of two foundational networking libraries:

- **RakNet** (2001-2014) — Industry-standard game networking library by Jenkins Software, used in countless multiplayer games. Acquired and open-sourced by Oculus VR.
- **SLikeNet** (2016-2019) — Community continuation by SLikeSoft that modernized RakNet with bug fixes, security patches, and C++11 support.

With SLikeNet no longer maintained, MafiaNet carries the torch forward—providing an actively developed, modern networking solution for the game development community.

## Community

- **Discord**: [Join our server](https://discord.gg/eBQ4QHX) for support and discussion
- **Issues**: [Report bugs](https://github.com/MafiaHub/MafiaNet/issues) or request features
- **Contributions**: Pull requests are welcome!

## License

MafiaNet is released under the [MIT License](LICENSE.md).

---

<div align="center">
  <sub>Built with passion by the <a href="https://github.com/MafiaHub">MafiaHub</a> community</sub>
</div>
