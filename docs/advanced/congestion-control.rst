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
   peer->Send(&bs, IMMEDIATE_PRIORITY, RELIABLE_ORDERED, 0, addr, false);

   // High priority (game state)
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, addr, false);

   // Medium priority (player updates)
   peer->Send(&bs, MEDIUM_PRIORITY, UNRELIABLE_SEQUENCED, 0, addr, false);

   // Low priority (chat, non-critical)
   peer->Send(&bs, LOW_PRIORITY, RELIABLE, 0, addr, false);

**Reliability types:**

* ``RELIABLE`` - Guaranteed delivery, any order
* ``RELIABLE_ORDERED`` - Guaranteed delivery, in order
* ``RELIABLE_SEQUENCED`` - Guaranteed, newest only
* ``UNRELIABLE`` - No guarantee, lowest overhead
* ``UNRELIABLE_SEQUENCED`` - No guarantee, newest only

Tuning Parameters
-----------------

.. code-block:: cpp

   // MTU discovery (finds optimal packet size)
   peer->SetMTUSize(1492);  // Default, good for most networks

   // Timeout settings
   peer->SetTimeoutTime(10000, remoteAddress);  // 10 seconds

   // Packet ordering channels (0-31)
   // Use different channels for independent data streams
   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, addr, false);  // Channel 0
   peer->Send(&bs2, HIGH_PRIORITY, RELIABLE_ORDERED, 1, addr, false); // Channel 1

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

See Also
--------

* :doc:`debugging-disconnects` - Timeout issues
* :doc:`preprocessor-directives` - Build options
* :doc:`../basics/bitstreams` - Efficient serialization
