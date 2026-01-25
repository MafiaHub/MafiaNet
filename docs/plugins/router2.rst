Router2 Plugin
==============

The Router2 plugin enables packet routing through intermediate systems when direct
connections are not possible. This is essential for NAT traversal fallback and
connecting players behind incompatible NAT types.

Basic Usage
-----------

**Setting up routing:**

.. code-block:: cpp

   #include "slikenet/Router2.h"

   MafiaNet::Router2 router2;
   peer->AttachPlugin(&router2);

   // Allow this system to act as a relay
   router2.SetMaximumForwardingRequests(64);

**Requesting a routed connection:**

.. code-block:: cpp

   // When direct connection fails, request routing through another peer
   if (directConnectionFailed) {
       // Try to connect through a known intermediate system
       router2.EstablishRouting(targetGuid);
   }

**Handling routing events:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       switch (packet->data[0]) {
           case ID_ROUTER_2_FORWARDING_ESTABLISHED:
               printf("Route established to %s\n",
                      packet->guid.ToString());
               // Can now send packets to this system
               break;

           case ID_ROUTER_2_FORWARDING_NO_PATH:
               printf("No routing path available\n");
               break;

           case ID_ROUTER_2_REROUTED:
               printf("Connection rerouted through new relay\n");
               break;
       }
       peer->DeallocatePacket(packet);
   }

**Sending through routes:**

.. code-block:: cpp

   // Packets sent normally will automatically use established routes
   MafiaNet::BitStream bs;
   bs.Write((MafiaNet::MessageID)ID_USER_PACKET_ENUM);
   bs.Write("Hello via relay!");

   peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetGuid, false);

Key Features
------------

* Transparent packet forwarding
* Automatic route discovery
* Multi-hop routing support
* Bandwidth limiting per forwarded connection
* Route failover on relay disconnect
* Compatible with all packet types
* Minimal latency overhead

Configuration Options
---------------------

* ``SetMaximumForwardingRequests()`` - Limit relay capacity
* ``EstablishRouting()`` - Request route to target
* ``SetSocketsPlugin()`` - Integration with socket layer
* ``GetForwardingRequestList()`` - View active forwards
* ``SetDebugInterface()`` - Enable debug logging

See Also
--------

* :doc:`../advanced/nat-traversal-architecture` - NAT solution overview
* :doc:`nat-type-detection` - NAT type identification
* :doc:`fully-connected-mesh-2` - P2P with routing fallback
