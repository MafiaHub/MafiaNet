Multiplayer Game Components
===========================

This guide covers the essential components of a multiplayer game.

Architecture Overview
---------------------

.. code-block:: text

   ┌─────────────────────────────────────────────────────────────┐
   │                     Your Game                               │
   ├─────────────────────────────────────────────────────────────┤
   │  Gameplay    │  Networking   │  Audio/Video  │  Input       │
   │  Logic       │  Layer        │               │              │
   ├──────────────┼───────────────┼───────────────┼──────────────┤
   │              │  MafiaNet     │               │              │
   │              │  ┌──────────┐ │               │              │
   │              │  │Plugins   │ │               │              │
   │              │  │Reliability│ │               │              │
   │              │  │Security  │ │               │              │
   │              │  └──────────┘ │               │              │
   └──────────────┴───────────────┴───────────────┴──────────────┘

Server Types
------------

**Dedicated Server**

Separate server process, no player:

- Best for competitive games
- Consistent performance
- Can be hosted in data centers

**Player-Hosted (Listen Server)**

One player acts as server:

- No server costs
- Host has latency advantage
- Requires host migration on disconnect

**Peer-to-Peer**

All players connect to each other:

- No single point of failure
- Complex synchronization
- Use FullyConnectedMesh2 plugin

State Synchronization
---------------------

**Authoritative Server**

Server owns all game state:

.. code-block:: cpp

   // Client sends input
   SendInput(moveDirection, shooting);

   // Server simulates and broadcasts result
   for (auto& player : players) {
       player.Update(deltaTime);
       BroadcastPlayerState(player);
   }

**Client-Side Prediction**

Client predicts locally, server corrects:

.. code-block:: cpp

   // Client: Apply input immediately
   ApplyInput(input);
   SaveState(sequenceNumber);

   // When server state arrives:
   if (serverSequence > lastConfirmed) {
       RewindTo(serverSequence);
       ReplayInputsFrom(serverSequence);
   }

Essential Systems
-----------------

**Lobby/Matchmaking**

- Player discovery
- Team assignment
- Ready state synchronization
- See :doc:`../plugins/lobby2`

**Object Replication**

- Spawn/destroy across network
- State synchronization
- See :doc:`../plugins/replica-manager-3`

**NAT Traversal**

- Connect players behind routers
- See :doc:`../plugins/nat-punchthrough`

**Voice Chat**

- Real-time voice communication
- See :doc:`../plugins/rakvoice`

Network Tick Rate
-----------------

Common configurations:

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Game Type
     - Tick Rate
     - Notes
   * - FPS
     - 60-128 Hz
     - High precision needed
   * - Racing
     - 30-60 Hz
     - Interpolation important
   * - Strategy
     - 10-20 Hz
     - Lower requirements
   * - Turn-based
     - Event-based
     - No continuous sync

Bandwidth Budget
----------------

Typical per-player bandwidth:

- **Position updates**: 20-50 bytes × tick rate
- **Input**: 5-10 bytes × input rate
- **Events**: Variable, bursty
- **Voice**: 2-4 KB/s when speaking

Example: 60 Hz shooter

.. code-block:: text

   Position: 30 bytes × 60 = 1.8 KB/s per player
   10 players = 18 KB/s upload (server)
   Client receives: 18 KB/s download

Latency Handling
----------------

**Interpolation**: Smooth movement between updates

**Extrapolation**: Predict future positions

**Lag Compensation**: Rewind time for hit detection

**Input Buffering**: Delay inputs to hide jitter

See Also
--------

* :doc:`system-overview` - Technical architecture
* :doc:`tutorial` - Hands-on tutorial
* :doc:`../guide/client-server` - Client-server patterns
* :doc:`../guide/peer-to-peer` - P2P networking
