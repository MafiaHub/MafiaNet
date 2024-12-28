<div align="center">
   <a href="https://github.com/MafiaHub/Framework"><img src="https://github.com/MafiaHub/Framework/assets/9026786/43e839f2-f207-47bf-aa59-72371e8403ba" alt="MafiaHub" /></a>
</div>

<div align="center">
    <a href="https://discord.gg/eBQ4QHX"><img src="https://img.shields.io/discord/402098213114347520.svg" alt="Discord server" /></a>
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/License-MafiaHub%20OSS-blue" alt="license" /></a>
</div>

<br />
<div align="center">
    Cross-platform network engine written in C++ and specifically designed for games.<br/>
    Based on RakNet and SLikeNet
</div>

## History

MafiaNet is a fork of SLikeNet, which itself was a continuation of RakNet, a network engine with over 13 years of active development. RakNet, originally developed by Jenkins Software LLC, was acquired by Oculus VR in 2014 and open-sourced. When development stagnated, SLikeSoft created SLikeNet to continue RakNet's legacy, implementing various bug fixes, security improvements, and modernizing the codebase to support newer compilers and language features.

## Introduction

Now, as SLikeNet itself is no longer maintained, MafiaNet continues this legacy by providing an actively maintained, modern networking solution for games and similar applications. MafiaNet maintains the core functionality while modernizing the codebase further, implementing new features, and ensuring compatibility with current development standards and security practices.

The engine supports Windows, Linux, and Mac as primary platforms, with limited support for iOS, Android, and Windows Phone. It is designed specifically for games and applications with similar networking requirements, providing a robust foundation for multiplayer game development.

## Major Changes

- [ ] Removed old-fashioned files
- [ ] Removed legacy and unmaintained nor used integrations
- [ ] Improved documentation and code samples
- [ ] Improved CMake support
- [ ] Updated dependencies
- [ ] Added unit tests
- [ ] Added github action pipelines
- [ ] Added proper github releases

## Key Features

### Core Networking
- Reliable UDP messaging system
- Automatic packet ordering and splitting
- IPv4 and IPv6 support
- Secure connections with SSL/TLS encryption
- Binary packet message system
- Connection management and statistics

### Multiplayer Features
- Peer-to-peer networking capabilities
- Host migration system
- NAT traversal (including NAT punchthrough)
- Cross-platform VOIP integration
- Room and lobby system
- Remote procedure calls (RPC)
- Object replication system
- Team management system

### Development Tools
- Built-in packet logger
- Network simulator for testing
- Comprehensive stats tracking
- Bandwidth limiter
- Message filtering system
- Ready-state manager

### Support Systems
- File transfer capabilities
- DirectoryDeltaTransfer for efficient patching
- Cloud computing interfaces
- Email sender
- Database integration (SQLite, MySQL, PostgreSQL)
- String compression
- Plugin architecture

### Utility Features
- Autopatcher system
- LAN server discovery
- Console server management
- Two-way authentication
- Router interface
- UPnP support