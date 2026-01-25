FullyConnectedMesh2 Plugin
==========================

The FullyConnectedMesh2 plugin manages peer-to-peer mesh networks where every
participant connects to every other participant. It handles host determination,
connection management, and mesh topology verification.

Basic Usage
-----------

**Setting up a mesh network:**

.. code-block:: cpp

   #include "slikenet/FullyConnectedMesh2.h"

   MafiaNet::FullyConnectedMesh2 fcm2;
   peer->AttachPlugin(&fcm2);

   // Set connection credentials (all peers must use same password)
   fcm2.SetConnectOnNewRemoteConnection(true, "gamePassword");

   // Start the mesh with initial host
   fcm2.StartVerifiedJoin(hostGuid);

**Handling mesh events:**

.. code-block:: cpp

   MafiaNet::Packet* packet;
   while ((packet = peer->Receive()) != nullptr) {
       switch (packet->data[0]) {
           case ID_FCM2_NEW_HOST:
               // A new host has been elected
               printf("New host: %s\n",
                      fcm2.GetHostSystem().ToString());
               break;

           case ID_FCM2_VERIFIED_JOIN_CAPABLE:
               // Can join the mesh
               fcm2.RespondOnVerifiedJoinCapable(packet, true,
                                                  UNASSIGNED_RAKNET_GUID);
               break;

           case ID_FCM2_VERIFIED_JOIN_ACCEPTED:
               printf("Successfully joined mesh\n");
               break;
       }
       peer->DeallocatePacket(packet);
   }

Key Features
------------

* Automatic host migration when host disconnects
* Deterministic host election (lowest GUID wins)
* Verified join process to prevent partial connections
* Connection state synchronization across all peers
* NAT punchthrough integration support
* Participant list management

Configuration Options
---------------------

* ``SetAutoparticipateConnections()`` - Auto-add new connections to mesh
* ``SetConnectOnNewRemoteConnection()`` - Auto-connect to peers of peers
* ``GetHostSystem()`` - Get current mesh host
* ``GetParticipantCount()`` - Number of mesh participants
* ``GetVerifiedJoinRequiredProcessingList()`` - Pending join verifications

See Also
--------

* :doc:`nat-type-detection` - NAT traversal for P2P
* :doc:`../advanced/nat-traversal-architecture` - Complete NAT solution
* :doc:`rpc4` - Remote procedure calls in mesh
