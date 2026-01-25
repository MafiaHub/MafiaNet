Statistics
==========

MafiaNet provides detailed statistics about network performance.

Getting Statistics
------------------

.. code-block:: cpp

   MafiaNet::RakNetStatistics stats;
   peer->GetStatistics(systemAddress, &stats);

   // Or for all connections combined
   MafiaNet::RakNetStatistics* stats = peer->GetStatistics(
       MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);

Key Statistics
--------------

Bandwidth
~~~~~~~~~

.. code-block:: cpp

   // Bytes sent/received per second (actual)
   uint64_t bytesSentPerSec = stats.valueOverLastSecond[
       MafiaNet::ACTUAL_BYTES_SENT];
   uint64_t bytesRecvPerSec = stats.valueOverLastSecond[
       MafiaNet::ACTUAL_BYTES_RECEIVED];

   // User data only (excluding protocol overhead)
   uint64_t userBytesSent = stats.valueOverLastSecond[
       MafiaNet::USER_MESSAGE_BYTES_SENT];

Packet Counts
~~~~~~~~~~~~~

.. code-block:: cpp

   // Total messages
   uint64_t messagesSent = stats.runningTotal[
       MafiaNet::USER_MESSAGE_BYTES_PUSHED];

   // Packets sent/received
   uint64_t packetsSent = stats.valueOverLastSecond[
       MafiaNet::ACTUAL_BYTES_SENT] / averagePacketSize;

Latency
~~~~~~~

.. code-block:: cpp

   // Get ping to a specific system
   int ping = peer->GetAveragePing(systemAddress);

   // Last ping value
   int lastPing = peer->GetLastPing(systemAddress);

   // Lowest ping recorded
   int lowestPing = peer->GetLowestPing(systemAddress);

Packet Loss
~~~~~~~~~~~

.. code-block:: cpp

   // Messages lost
   uint64_t messagesLost = stats.runningTotal[
       MafiaNet::USER_MESSAGE_BYTES_RESENT] /
       averageMessageSize;  // Approximate

   // Packet loss percentage
   float packetLossPercent = stats.packetlossLastSecond;

Connection Quality
~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Bandwidth exceeded (sending too fast)
   bool bandwidthExceeded = stats.bandwidthExceededStatistic;

   // Connection start time
   MafiaNet::Time connectionTime = stats.connectionStartTime;

Statistics Fields Reference
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Field
     - Description
   * - ``ACTUAL_BYTES_SENT``
     - Total bytes sent including overhead
   * - ``ACTUAL_BYTES_RECEIVED``
     - Total bytes received
   * - ``USER_MESSAGE_BYTES_SENT``
     - User data bytes sent
   * - ``USER_MESSAGE_BYTES_RECEIVED``
     - User data bytes received
   * - ``USER_MESSAGE_BYTES_PUSHED``
     - Bytes queued for sending
   * - ``USER_MESSAGE_BYTES_RESENT``
     - Bytes resent due to packet loss
   * - ``packetlossLastSecond``
     - Packet loss percentage (0.0-1.0)
   * - ``bandwidthExceededStatistic``
     - True if sending faster than bandwidth

Displaying Statistics
---------------------

.. code-block:: cpp

   void DisplayNetworkStats(MafiaNet::RakPeerInterface* peer,
                           MafiaNet::SystemAddress addr) {
       MafiaNet::RakNetStatistics stats;
       peer->GetStatistics(addr, &stats);

       printf("=== Network Statistics ===\n");
       printf("Ping: %d ms\n", peer->GetAveragePing(addr));
       printf("Packet Loss: %.1f%%\n", stats.packetlossLastSecond * 100);
       printf("Bytes Sent/sec: %llu\n",
              stats.valueOverLastSecond[MafiaNet::ACTUAL_BYTES_SENT]);
       printf("Bytes Recv/sec: %llu\n",
              stats.valueOverLastSecond[MafiaNet::ACTUAL_BYTES_RECEIVED]);

       if (stats.bandwidthExceededStatistic) {
           printf("WARNING: Bandwidth exceeded!\n");
       }
   }

Monitoring Connection Health
----------------------------

.. code-block:: cpp

   enum ConnectionQuality {
       EXCELLENT,  // < 50ms, < 1% loss
       GOOD,       // < 100ms, < 5% loss
       FAIR,       // < 200ms, < 10% loss
       POOR        // > 200ms or > 10% loss
   };

   ConnectionQuality GetConnectionQuality(MafiaNet::RakPeerInterface* peer,
                                         MafiaNet::SystemAddress addr) {
       int ping = peer->GetAveragePing(addr);

       MafiaNet::RakNetStatistics stats;
       peer->GetStatistics(addr, &stats);
       float loss = stats.packetlossLastSecond;

       if (ping < 50 && loss < 0.01f) return EXCELLENT;
       if (ping < 100 && loss < 0.05f) return GOOD;
       if (ping < 200 && loss < 0.10f) return FAIR;
       return POOR;
   }

StatisticsHistory Plugin
------------------------

For tracking statistics over time, use the StatisticsHistory plugin:

.. code-block:: cpp

   #include "mafianet/StatisticsHistory.h"

   MafiaNet::StatisticsHistory* statsHistory =
       MafiaNet::StatisticsHistory::GetInstance();
   peer->AttachPlugin(statsHistory);

   // Later, query historical data
   // ...

See Also
--------

* :doc:`../advanced/debugging-disconnects` - Diagnosing connection issues
* :doc:`reliability-types` - Impact on bandwidth usage
