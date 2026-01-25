Debugging Disconnects
=====================

Connection issues are common in networked games. This guide provides systematic
approaches to diagnosing and resolving disconnection problems.

Common Disconnect Causes
------------------------

1. **Timeout** - No packets received within timeout period
2. **Connection request failed** - Server rejected or unreachable
3. **NAT issues** - Firewall blocking or NAT incompatibility
4. **Bandwidth saturation** - Network overwhelmed
5. **Application bugs** - Improper connection handling

Diagnostic Tools
----------------

**Enable packet logging:**

.. code-block:: cpp

   #include "slikenet/PacketLogger.h"

   MafiaNet::PacketLogger logger;
   peer->AttachPlugin(&logger);
   logger.SetLogFile("network_debug.log");

**Monitor statistics:**

.. code-block:: cpp

   void PrintConnectionStats(MafiaNet::RakPeerInterface* peer,
                             MafiaNet::SystemAddress addr) {
       MafiaNet::RakNetStatistics stats;
       peer->GetStatistics(addr, &stats);

       printf("=== Connection Stats ===\n");
       printf("Ping: %d ms\n", peer->GetAveragePing(addr));
       printf("Packet loss: %.2f%%\n", stats.packetlossLastSecond * 100.0f);
       printf("Messages pending: %d\n", stats.messageInSendBuffer);
       printf("Bytes pending: %llu\n", stats.bytesInSendBuffer);
       printf("Last send: %llu ms ago\n", stats.connectionStartTime);
   }

**Check connection state:**

.. code-block:: cpp

   MafiaNet::ConnectionState state = peer->GetConnectionState(addr);
   switch (state) {
       case IS_PENDING:
           printf("Connection pending\n");
           break;
       case IS_CONNECTING:
           printf("Currently connecting\n");
           break;
       case IS_CONNECTED:
           printf("Connected\n");
           break;
       case IS_DISCONNECTING:
           printf("Disconnecting\n");
           break;
       case IS_NOT_CONNECTED:
           printf("Not connected\n");
           break;
   }

Handling Disconnect Packets
---------------------------

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       switch (packet->data[0]) {
           case ID_DISCONNECTION_NOTIFICATION:
               printf("Clean disconnect from %s\n",
                      packet->systemAddress.ToString());
               break;

           case ID_CONNECTION_LOST:
               printf("Connection lost (timeout) from %s\n",
                      packet->systemAddress.ToString());
               // Investigate timeout cause
               break;

           case ID_CONNECTION_ATTEMPT_FAILED:
               printf("Failed to connect to %s\n",
                      packet->systemAddress.ToString());
               break;

           case ID_NO_FREE_INCOMING_CONNECTIONS:
               printf("Server full\n");
               break;

           case ID_CONNECTION_BANNED:
               printf("Banned from server\n");
               break;
       }
       peer->DeallocatePacket(packet);
   }

Timeout Tuning
--------------

.. code-block:: cpp

   // Increase timeout for high-latency connections
   peer->SetTimeoutTime(30000, UNASSIGNED_SYSTEM_ADDRESS);  // 30 seconds

   // Check for unresponsive connections periodically
   void CheckConnections(MafiaNet::RakPeerInterface* peer) {
       MafiaNet::SystemAddress systems[32];
       unsigned short count = 32;
       peer->GetConnectionList(systems, &count);

       for (int i = 0; i < count; i++) {
           int ping = peer->GetLastPing(systems[i]);
           if (ping > 5000) {
               printf("Warning: High ping to %s: %d ms\n",
                      systems[i].ToString(), ping);
           }
       }
   }

Troubleshooting Checklist
-------------------------

1. **Verify network reachability** - Can you ping the server?
2. **Check firewall rules** - Is the port open?
3. **Verify port binding** - Is another process using the port?
4. **Check NAT type** - Use NatTypeDetection plugin
5. **Monitor bandwidth** - Check for saturation
6. **Review packet logs** - Look for patterns before disconnect

See Also
--------

* :doc:`../plugins/packet-logger` - Detailed logging
* :doc:`congestion-control` - Bandwidth management
* :doc:`nat-traversal-architecture` - NAT issues
