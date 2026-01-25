Frequently Asked Questions
==========================

This page addresses common questions about MafiaNet development and deployment.

General
-------

**What is MafiaNet?**

MafiaNet is a cross-platform network engine for multiplayer games, forked from
SLikeNet (which continued RakNet). It provides reliable UDP messaging, NAT
traversal, and game-specific networking features.

**What platforms are supported?**

* Windows (Visual Studio 2019+)
* Linux (GCC 9+, Clang 10+)
* macOS (Xcode 12+)

**Is MafiaNet free to use?**

Yes, MafiaNet uses a permissive license allowing both commercial and
non-commercial use.

Networking
----------

**How do I choose between TCP and UDP?**

Use UDP (RakPeerInterface) for:

* Real-time game state updates
* Low-latency requirements
* Custom reliability needs

Use TCP (TCPInterface) for:

* File downloads
* Web API calls
* Database connections

**What's the maximum packet size?**

Default MTU is 1492 bytes. Larger messages are automatically fragmented and
reassembled. For best performance, keep frequent messages under MTU.

**How many connections can I handle?**

Tested with 1000+ simultaneous connections. Actual limits depend on:

* Server hardware
* Message frequency
* Game logic complexity

NAT Traversal
-------------

**Why can't players connect to each other?**

Common causes:

1. Both behind symmetric NAT (use relay)
2. Firewall blocking ports
3. NAT punchthrough server not running

See :doc:`../advanced/nat-traversal-architecture` for solutions.

**Do I need a dedicated server?**

For NAT punchthrough, yes - a facilitator server on a public IP is required.
The actual game traffic can be peer-to-peer after punchthrough succeeds.

**What ports do I need to open?**

* Your game's UDP port (configurable)
* For servers: ensure port is forwarded in router

Performance
-----------

**How do I reduce bandwidth?**

1. Use appropriate reliability (UNRELIABLE for non-critical data)
2. Compress strings with StringCompressor
3. Use BitStream efficiently (WriteBits, not WriteBytes)
4. Reduce update frequency for non-critical data

**Why am I getting high latency?**

Check:

1. Message priority settings
2. Congestion control (may be throttling)
3. Message ordering channels
4. Server tick rate

See :doc:`../advanced/congestion-control`.

**How do I handle packet loss?**

* Use RELIABLE/RELIABLE_ORDERED for critical data
* Use UNRELIABLE_SEQUENCED for real-time updates
* Monitor statistics with GetStatistics()

Security
--------

**How do I prevent cheating?**

* Validate all client input server-side
* Use TwoWayAuthentication for login
* Use MessageFilter to restrict unauthorized actions
* Never trust client-computed results

**Is traffic encrypted?**

Not by default. Enable with ``_RAKNET_SECURE_CONNECTIONS`` define or use
application-layer encryption.

Troubleshooting
---------------

**Connection attempts always fail**

1. Verify server is running and reachable
2. Check firewall settings
3. Confirm port numbers match
4. Enable PacketLogger for diagnostics

**Memory usage keeps growing**

1. Ensure packets are deallocated
2. Check for plugin memory leaks
3. Use custom memory management to track

See Also
--------

* :doc:`programming-tips` - Best practices
* :doc:`../advanced/debugging-disconnects` - Connection issues
* :doc:`../guide/concepts` - Core concepts
