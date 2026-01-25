System Overview
===============

This page provides a high-level overview of MafiaNet's architecture.

Core Components
---------------

.. code-block:: text

   ┌─────────────────────────────────────────────────────────┐
   │                    Your Application                     │
   ├─────────────────────────────────────────────────────────┤
   │                    Plugin Layer                         │
   │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   │
   │  │Replica   │ │NAT Punch │ │File      │ │ RPC4     │   │
   │  │Manager3  │ │through   │ │Transfer  │ │          │   │
   │  └──────────┘ └──────────┘ └──────────┘ └──────────┘   │
   ├─────────────────────────────────────────────────────────┤
   │                  RakPeerInterface                       │
   │  ┌──────────────────────────────────────────────────┐  │
   │  │ Connection Management | Reliability Layer        │  │
   │  │ BitStream            | Security                  │  │
   │  └──────────────────────────────────────────────────┘  │
   ├─────────────────────────────────────────────────────────┤
   │                    Socket Layer                         │
   │  ┌──────────────────────────────────────────────────┐  │
   │  │              UDP Sockets                         │  │
   │  └──────────────────────────────────────────────────┘  │
   └─────────────────────────────────────────────────────────┘

RakPeerInterface
----------------

The main entry point for all networking. Handles:

- Socket creation and management
- Connection establishment and maintenance
- Packet sending and receiving
- Plugin management
- Statistics tracking

Reliability Layer
-----------------

Provides reliable delivery over UDP:

- **Sequencing**: Packets arrive in order
- **Acknowledgments**: Confirms packet delivery
- **Retransmission**: Resends lost packets
- **Congestion control**: Adapts to network conditions

BitStream
---------

Binary serialization utility:

- Write any data type
- Bit-level precision
- Endian handling
- Compression helpers

Plugin System
-------------

Modular extensions that hook into the network layer:

- Process packets before/after your application
- Add features without modifying core code
- Enable/disable as needed

Data Flow
---------

Sending
~~~~~~~

.. code-block:: text

   Your App → BitStream → RakPeerInterface → Reliability Layer → Socket → Network

1. Create data with BitStream
2. Call ``Send()`` with priority and reliability
3. Reliability layer queues and manages delivery
4. Socket sends UDP packets

Receiving
~~~~~~~~~

.. code-block:: text

   Network → Socket → Reliability Layer → Plugins → RakPeerInterface → Your App

1. Socket receives UDP packets
2. Reliability layer reassembles and orders
3. Plugins process (optional)
4. ``Receive()`` returns packet to your app

Threading Model
---------------

MafiaNet uses a background thread for:

- Socket I/O
- Reliability processing
- Keep-alive pings

Your application:

- Calls ``Receive()`` from any thread
- Calls ``Send()`` from any thread
- Both are thread-safe

Memory Management
-----------------

- **Packets**: Allocated by MafiaNet, freed with ``DeallocatePacket()``
- **BitStream**: You manage (stack or heap)
- **Plugins**: You manage lifetime
- **RakPeerInterface**: Create with ``GetInstance()``, destroy with ``DestroyInstance()``

Namespaces
----------

- ``MafiaNet::`` - Primary namespace for all classes
- ``RakNet::`` - Legacy alias (for backward compatibility)
- ``DataStructures::`` - Container classes (DS_List, DS_Queue, etc.)

Common Patterns
---------------

Client-Server
~~~~~~~~~~~~~

One authoritative server, multiple clients:

.. code-block:: text

   Client A ─────┐
                 │
   Client B ─────┼───── Server
                 │
   Client C ─────┘

Peer-to-Peer
~~~~~~~~~~~~

All peers connect to each other:

.. code-block:: text

   Peer A ◄────► Peer B
      │  ╲      ╱  │
      │   ╲    ╱   │
      │    ╲  ╱    │
      │     ╲╱     │
      │     ╱╲     │
      │    ╱  ╲    │
      │   ╱    ╲   │
      ▼  ╱      ╲  ▼
   Peer C ◄────► Peer D

Hybrid
~~~~~~

Central server for matchmaking, P2P for gameplay:

.. code-block:: text

   Phase 1: Matchmaking          Phase 2: Gameplay

   Client A ──┐                  Client A ◄──► Client B
              │
   Client B ──┼── Lobby Server   (direct connection)
              │
   Client C ──┘

See Also
--------

* :doc:`quickstart` - Get started coding
* :doc:`../basics/startup` - Startup details
* :doc:`../guide/concepts` - Core concepts explained
