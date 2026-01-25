NATTypeDetection Plugin
=======================

The NATTypeDetection plugin determines the NAT (Network Address Translation) type
of the local router. This information is useful for predicting connection success
rates and choosing appropriate NAT traversal strategies.

Basic Usage
-----------

**Client-side detection:**

.. code-block:: cpp

   #include "slikenet/NatTypeDetectionClient.h"

   MafiaNet::NatTypeDetectionClient natClient;
   peer->AttachPlugin(&natClient);

   // Connect to detection server (must have 3+ IP addresses)
   natClient.DetectNATType(serverAddress);

**Handling detection results:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       if (packet->data[0] == ID_NAT_TYPE_DETECTION_RESULT) {
           MafiaNet::NATTypeDetectionResult result =
               (MafiaNet::NATTypeDetectionResult)packet->data[1];

           switch (result) {
               case NAT_TYPE_NONE:
                   printf("No NAT detected (public IP)\n");
                   break;
               case NAT_TYPE_FULL_CONE:
                   printf("Full cone NAT - easiest to traverse\n");
                   break;
               case NAT_TYPE_ADDRESS_RESTRICTED:
                   printf("Address restricted NAT\n");
                   break;
               case NAT_TYPE_PORT_RESTRICTED:
                   printf("Port restricted NAT\n");
                   break;
               case NAT_TYPE_SYMMETRIC:
                   printf("Symmetric NAT - hardest to traverse\n");
                   break;
           }
       }
       peer->DeallocatePacket(packet);
   }

**Server-side setup:**

.. code-block:: cpp

   #include "slikenet/NatTypeDetectionServer.h"

   // Server requires 3 different IP addresses
   MafiaNet::NatTypeDetectionServer natServer;
   peer->AttachPlugin(&natServer);

Key Features
------------

* Identifies 5 NAT types: None, Full Cone, Address Restricted, Port Restricted, Symmetric
* Predicts NAT punchthrough success probability
* Works with standard STUN-like detection algorithm
* Server component for self-hosted detection

NAT Type Compatibility
----------------------

+-------------------+------+------+------+------+------+
| NAT Types         | None | Full | Addr | Port | Sym  |
+===================+======+======+======+======+======+
| None              | Yes  | Yes  | Yes  | Yes  | Yes  |
+-------------------+------+------+------+------+------+
| Full Cone         | Yes  | Yes  | Yes  | Yes  | Yes  |
+-------------------+------+------+------+------+------+
| Address Restricted| Yes  | Yes  | Yes  | Yes  | No   |
+-------------------+------+------+------+------+------+
| Port Restricted   | Yes  | Yes  | Yes  | Yes  | No   |
+-------------------+------+------+------+------+------+
| Symmetric         | Yes  | Yes  | No   | No   | No   |
+-------------------+------+------+------+------+------+

See Also
--------

* :doc:`../advanced/nat-traversal-architecture` - Complete NAT solution
* :doc:`fully-connected-mesh-2` - P2P mesh with NAT traversal
* :doc:`router2` - Relay fallback for failed punchthrough
