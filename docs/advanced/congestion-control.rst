Congestion Control
==================

MafiaNet implements adaptive congestion control to maximize throughput while
avoiding network congestion. Understanding and tuning these settings is essential
for optimal game networking performance.

Overview
--------

The congestion control system:

* Monitors packet loss and round-trip times
* Adjusts send rate dynamically
* Prioritizes reliable over unreliable traffic
* Supports different algorithms for different use cases

Basic Configuration
-------------------

**Setting bandwidth limits:**

.. code-block:: cpp

   MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();

   // Limit outgoing bandwidth (bytes per second)
   peer->SetPerConnectionOutgoingBandwidthLimit(50000);  // 50 KB/s

   // Set unlimited (default)
   peer->SetPerConnectionOutgoingBandwidthLimit(0);

**Checking connection quality:**

.. code-block:: cpp

   MafiaNet::RakNetStatistics stats;
   peer->GetStatistics(remoteAddress, &stats);

   printf("RTT: %d ms\n", peer->GetAveragePing(remoteAddress));
   printf("Packet loss: %.2f%%\n", stats.packetlossLastSecond * 100.0f);
   printf("Bandwidth: %d bytes/sec\n", stats.BPSLimitByCongestionControl);

Algorithm Selection
-------------------

Configure in ``RakNetDefinesOverrides.h``:

.. code-block:: cpp

   // Sliding window (TCP-like, recommended for most games)
   #define USE_SLIDING_WINDOW_CONGESTION_CONTROL 1

   // Fixed rate (no congestion control)
   #define USE_SLIDING_WINDOW_CONGESTION_CONTROL 0

Priority and Reliability
------------------------

**Message priorities:**

.. code-block:: cpp

   // Immediate send (highest priority)
   peer->Send(&bs, MafiaNet::Priority::Immediate, MafiaNet::Reliability::ReliableOrdered, 0, addr, false);

   // High priority (game state)
   peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, addr, false);

   // Medium priority (player updates)
   peer->Send(&bs, MafiaNet::Priority::Medium, MafiaNet::Reliability::UnreliableSequenced, 0, addr, false);

   // Low priority (chat, non-critical)
   peer->Send(&bs, MafiaNet::Priority::Low, MafiaNet::Reliability::Reliable, 0, addr, false);

**Reliability types:**

* ``MafiaNet::Reliability::Reliable`` - Guaranteed delivery, any order
* ``MafiaNet::Reliability::ReliableOrdered`` - Guaranteed delivery, in order
* ``MafiaNet::Reliability::ReliableSequenced`` - Guaranteed, newest only
* ``MafiaNet::Reliability::Unreliable`` - No guarantee, lowest overhead
* ``MafiaNet::Reliability::UnreliableSequenced`` - No guarantee, newest only

Tuning Parameters
-----------------

.. code-block:: cpp

   // MTU is negotiated per connection during the handshake -- the connecting peer
   // probes 1400, 1280, 1024 then 576 and keeps the largest size the path passed.
   // There is no setter; read the result back with:
   peer->GetMTUSize(remoteAddress);

   // The ceiling is compile-time. Lower MAXIMUM_MTU_SIZE (mafianet/mtu_size.h) if
   // your peers sit behind tunnelling with more overhead than 1400 leaves room for.

   // Timeout settings
   peer->SetTimeoutTime(10000, remoteAddress);  // 10 seconds

   // Packet ordering channels (0-31)
   // Use different channels for independent data streams
   peer->Send(&bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, addr, false);  // Channel 0
   peer->Send(&bs2, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 1, addr, false); // Channel 1

MTU Black Holes
---------------

The handshake probes the path MTU in one direction only. A tunnel that drops, rather than
fragments, datagrams over its ceiling on the other direction (OpenVPN and friends) lets the
connection establish and then black-holes its large datagrams. The reliability layer detects this
and steps the connection's MTU down one rung of the same ladder (1400, 1280, 1024, 576),
re-splitting every queued message that no longer fits. ``GetMTUSize()`` reports the new size and
``RakNetStatistics::mtuStepDowns`` counts the steps. A step is permanent for the connection.

It steps down only on a size-specific signature: a packet that would shrink at the next rung has
gone unacknowledged through 4 transmissions, and 12 transmissions of such large packets in a row
have failed without a datagram of their size being acknowledged in between. Ordinary loss hits
datagrams of every size, so on a lossy link some large datagrams still get through and the count
starts over; a black hole drops them all. A lone large packet takes 12 resends to detect, a
message with several large fragments in flight about as many resend rounds as it has large
fragments.

Messages re-split after a step-down are still delivered exactly once, and leave no gap in the
receiver's reliable-message window. The original's unacknowledged packets are dropped with their
reliable message numbers, and the receiver may have none, some or all of them: a step-down can be
a false positive, where the original arrived and only its acks were lost. So the re-split copy
starts with a header naming those numbers. The receiver marks them received, and delivers the copy
only if one of them had not arrived, which means the original can no longer complete. If all of
them had arrived, the original was delivered and the copy is dropped.

Statistics Monitoring
---------------------

.. code-block:: cpp

   MafiaNet::RakNetStatistics stats;
   peer->GetStatistics(addr, &stats);

   // Key metrics
   stats.messageInSendBuffer;        // Pending messages
   stats.bytesInSendBuffer;          // Pending bytes
   stats.packetlossLastSecond;       // Recent packet loss
   stats.BPSLimitByCongestionControl; // Current bandwidth limit
   stats.mtuStepDowns;               // MTU black-hole step-downs

See Also
--------

* :doc:`debugging-disconnects` - Timeout issues
* :doc:`preprocessor-directives` - Build options
* :doc:`../basics/bitstreams` - Efficient serialization
